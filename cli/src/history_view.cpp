#include "history_view.h"

#include "code_blocks.h"
#include "markdown_view.h"

#include "chatbot/web_search.h"

#include <ftxui/dom/node.hpp>
#include <ftxui/dom/requirement.hpp>
#include <ftxui/screen/box.hpp>
#include <ftxui/screen/color.hpp>
#include <ftxui/screen/string.hpp>

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace chatbot::cli {

namespace {

/// Etiqueta decorada ("Tú:", "(cancelada)"...) en un hbox: dentro del vbox
/// de la entrada se estiraría a todo el ancho, y el estilo con ella.
ftxui::Element label(ftxui::Element element) { return ftxui::hbox({std::move(element)}); }

bool same_sources(const std::vector<SearchResult>& a, const std::vector<SearchResult>& b) {
    return std::equal(a.begin(), a.end(), b.begin(), b.end(),
                      [](const SearchResult& x, const SearchResult& y) {
                          return x.title == y.title && x.url == y.url &&
                                 x.published_date == y.published_date;
                      });
}

bool same_entry(const Entry& a, const Entry& b) {
    return a.kind == b.kind && a.in_progress == b.in_progress && a.incomplete == b.incomplete &&
           a.cancelled == b.cancelled && a.text == b.text && a.note == b.note &&
           same_sources(a.sources, b.sources);
}

/// Texto filtrado (md::sanitize) en una sola línea.
std::string one_line(std::string_view text) {
    std::string out = md::sanitize(text);
    std::replace(out.begin(), out.end(), '\n', ' ');
    return out;
}

/// El bloque de fuentes como un párrafo de markdown armado a mano (sin
/// parsear: un título no puede meter markdown): "[n] título — fecha (url)",
/// una fuente por línea (sin fecha si no hay), con el título, la fecha y la
/// URL como enlace. Sin título: "[n] url — fecha". md::render codifica la URL
/// con hyperlink_target. Segunda defensa (TavilySearch y el lector de
/// archivos ya las descartan): una URL que no pasa is_web_url se dibuja como
/// texto plano, sin enlace.
md::Document sources_document(const std::vector<SearchResult>& sources) {
    md::Block paragraph;
    paragraph.kind = md::Block::Kind::Paragraph;
    for (std::size_t i = 0; i < sources.size(); ++i) {
        if (i > 0) {
            md::Run line_break;
            line_break.kind = md::Run::Kind::LineBreak;
            paragraph.runs.push_back(std::move(line_break));
        }
        const std::string url = one_line(sources[i].url);
        const std::string title = one_line(sources[i].title);
        const std::string date = one_line(sources[i].published_date);
        md::Run number;
        number.text = "[" + std::to_string(i + 1) + "] ";
        paragraph.runs.push_back(std::move(number));
        if (!is_web_url(sources[i].url)) {
            md::Run plain;
            plain.text = title.empty() ? url : title;
            if (!date.empty()) {
                plain.text += " — " + date;
            }
            if (!title.empty()) {
                plain.text += " (" + url + ")";
            }
            paragraph.runs.push_back(std::move(plain));
            continue;
        }
        md::Run link;
        link.text = title.empty() ? url : title;
        link.link = static_cast<int>(paragraph.links.size());
        paragraph.links.push_back(md::Link{url, false});
        md::Run dated;
        dated.text = date.empty() ? std::string{} : " — " + date;
        if (!title.empty()) {
            // Dentro del enlace: md::render agrega " (url)" al cerrarlo.
            link.text += dated.text;
        }
        paragraph.runs.push_back(std::move(link));
        if (title.empty() && !dated.text.empty()) {
            // El texto del enlace ya es la URL: la fecha va fuera, sin repetirla.
            paragraph.runs.push_back(std::move(dated));
        }
    }
    md::Document document;
    document.blocks.push_back(std::move(paragraph));
    return document;
}

/// Nodo que muestra una entrada ya dibujada: copia solo las celdas que caen
/// dentro de la zona visible (el stencil que deja yframe), así que el costo
/// de cada cuadro no crece con el largo del historial.
class Picture : public ftxui::Node {
public:
    explicit Picture(std::shared_ptr<const ftxui::Screen> image) : image_(std::move(image)) {}

    void ComputeRequirement() override {
        requirement_ = ftxui::Requirement{};
        requirement_.min_x = image_->dimx();
        requirement_.min_y = image_->dimy();
    }

    void Render(ftxui::Screen& screen) override {
        const int y_begin = std::max(box_.y_min, screen.stencil.y_min);
        const int y_end = std::min({box_.y_max, screen.stencil.y_max, box_.y_min + image_->dimy() - 1});
        const int x_begin = std::max(box_.x_min, screen.stencil.x_min);
        const int x_end = std::min({box_.x_max, screen.stencil.x_max, box_.x_min + image_->dimx() - 1});
        for (int y = y_begin; y <= y_end; ++y) {
            for (int x = x_begin; x <= x_end; ++x) {
                ftxui::Cell cell = image_->CellAt(x - box_.x_min, y - box_.y_min);
                // Los bordes ya se fusionaron al dibujar la imagen; si no se
                // apaga, se fusionarían con lo que rodea al historial.
                cell.automerge = false;
                if (cell.hyperlink != 0) {
                    // Los identificadores de enlace son de cada pantalla.
                    cell.hyperlink = screen.RegisterHyperlink(image_->Hyperlink(cell.hyperlink));
                }
                screen.CellAt(x, y) = std::move(cell);
            }
        }
    }

private:
    std::shared_ptr<const ftxui::Screen> image_;
};

} // namespace

const md::Document& HistoryView::document_for(Cached& cached, const std::string& text) {
    if (!cached.parsed || cached.source != text) {
        cached.document = md::parse(text);
        cached.source = text;
        cached.parsed = true;
        ++parse_count_;
    }
    return cached.document;
}

ftxui::Element HistoryView::entry_element(Cached& cached, const Entry& entry, int width,
                                          const Palette& palette, int first_code) {
    const ftxui::Decorator notice = palette.ink(&Theme::notice);
    switch (entry.kind) {
    case EntryKind::User:
        return ftxui::vbox(
            {label(ftxui::text("Tú:") | ftxui::bold | palette.ink(&Theme::user_label)),
             md::render_plain(entry.text, width)});
    case EntryKind::Assistant: {
        ftxui::Elements lines{
            label(ftxui::text("Asistente:") | ftxui::bold | palette.ink(&Theme::assistant_label)),
            md::render(document_for(cached, entry.text), width, palette, first_code,
                       first_code > 0 ? &cached.code_boxes : nullptr)};
        if (entry.cancelled) {
            lines.push_back(label(ftxui::text("(cancelada)") | notice));
        } else if (entry.incomplete) {
            lines.push_back(label(ftxui::text("(respuesta incompleta)") | notice));
        } else if (!entry.note.empty()) {
            lines.push_back(md::render_plain(entry.note, width, notice));
        }
        return ftxui::vbox(std::move(lines));
    }
    case EntryKind::Error:
        return md::render_plain(entry.text, width, palette.ink(&Theme::error));
    case EntryKind::Notice:
        return md::render_plain(entry.text, width, notice);
    case EntryKind::Sources:
        return ftxui::vbox({label(ftxui::text("Fuentes:") | ftxui::bold | notice),
                            md::render(sources_document(entry.sources), width, palette)});
    }
    return ftxui::text("");
}

const std::vector<CodeFrame>& HistoryView::code_frames(std::size_t entry) const {
    static const std::vector<CodeFrame> kNone;
    return entry < cache_.size() ? cache_[entry].code_frames : kNone;
}

ftxui::Element HistoryView::render(const std::vector<Entry>& entries, int width,
                                   const Palette& palette,
                                   const std::vector<int>& first_code_numbers) {
    width = std::max(width, 1);
    const std::string palette_key = palette.key();
    // Si cambió la conversación, las entradas se comparan por contenido: las
    // iguales se reusan y las demás se vuelven a dibujar.
    cache_.resize(entries.size());
    ftxui::Elements rows;
    rows.reserve(entries.size() * 2);
    for (std::size_t i = 0; i < entries.size(); ++i) {
        Cached& cached = cache_[i];
        const Entry& entry = entries[i];
        const int first_code = i < first_code_numbers.size() ? first_code_numbers[i] : 0;
        if (!cached.image || cached.width != width || cached.palette_key != palette_key ||
            cached.first_code != first_code || !same_entry(cached.drawn, entry)) {
            // Con el fondo y el texto de la paleta: Picture copia las celdas
            // tal cual, también su fondo.
            cached.code_boxes.clear();
            ftxui::Element element =
                entry_element(cached, entry, width, palette, first_code) | palette.base();
            // El ajuste de líneas es propio (sin flexbox), así que el alto
            // mínimo es el alto final: no hace falta Dimension::Fit.
            element->ComputeRequirement();
            const int height = std::max(element->requirement().min_y, 1);
            auto image = std::make_shared<ftxui::Screen>(width, height);
            ftxui::Render(*image, element);
            // Render dejó las cajas de los marcos en coordenadas de la imagen,
            // que son las de la entrada.
            cached.code_frames.clear();
            if (!cached.code_boxes.empty()) {
                const std::vector<CodeBlock> blocks = code_blocks_of(cached.document);
                for (std::size_t b = 0; b < cached.code_boxes.size() && b < blocks.size(); ++b) {
                    const int number = first_code + static_cast<int>(b);
                    cached.code_frames.push_back(
                        CodeFrame{number, cached.code_boxes[b],
                                  ftxui::string_width(md::code_title(number, blocks[b].info))});
                }
            }
            cached.image = std::move(image);
            cached.width = width;
            cached.palette_key = palette_key;
            cached.first_code = first_code;
            cached.drawn = entry;
            ++draw_count_;
        }
        if (!rows.empty()) {
            rows.push_back(ftxui::text(""));
        }
        rows.push_back(std::make_shared<Picture>(cached.image));
    }
    return ftxui::vbox(std::move(rows));
}

} // namespace chatbot::cli
