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

/// Etiquetas de los botones de un bloque: completas o compactas.
struct ButtonLabels {
    std::string_view copy;
    std::string_view save;
};
constexpr ButtonLabels kFullLabels{"[Copiar]", "[Guardar]"};
constexpr ButtonLabels kShortLabels{"[C]", "[G]"};
/// Después de copiar, el botón de copiar cambia a estas.
constexpr ButtonLabels kFullCopied{"[✓ Copiado]", "[Guardar]"};
constexpr ButtonLabels kShortCopied{"[✓]", "[G]"};
/// Columnas libres entre el título del marco y los botones.
constexpr int kTitleMargin = 2;

int labels_width(const ButtonLabels& labels) {
    return ftxui::string_width(std::string{labels.copy}) + 1 +
           ftxui::string_width(std::string{labels.save});
}

/// Las etiquetas que caben en un marco de frame_width columnas con un título
/// de title_width: los botones terminan una columna antes del borde derecho
/// y empiezan al menos kTitleMargin columnas después del título (que va
/// desde la columna 1). nullopt si ni las compactas caben. copied: con
/// [✓ Copiado], que también tiene que caber.
std::optional<ButtonLabels> fitting_labels(int frame_width, int title_width, bool copied) {
    for (const ButtonLabels& labels :
         {copied ? kFullCopied : kFullLabels, copied ? kShortCopied : kShortLabels}) {
        const int start = frame_width - 1 - labels_width(labels);
        if (start >= 1 + title_width + kTitleMargin) {
            return labels;
        }
    }
    return std::nullopt;
}

} // namespace

/// Nodo que muestra una entrada ya dibujada: copia solo las celdas que caen
/// dentro de la zona visible (el stencil que deja yframe), así que el costo
/// de cada cuadro no crece con el largo del historial. Después dibuja los
/// botones de sus bloques de código, que dependen de la zona visible y del
/// puntero, no de la entrada.
class HistoryView::Picture : public ftxui::Node {
public:
    /// hits: donde se anotan los botones dibujados (HistoryView::hits_).
    Picture(std::shared_ptr<const ftxui::Screen> image, std::vector<CodeFrame> frames,
            const HistoryView& view, std::vector<ButtonHit>* hits)
        : image_(std::move(image)), frames_(std::move(frames)), view_(view), hits_(hits) {}

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
        if (view_.buttons_visible_) {
            for (const CodeFrame& frame : frames_) {
                draw_buttons(screen, frame);
            }
        }
    }

private:
    void draw_buttons(ftxui::Screen& screen, const CodeFrame& frame) const {
        const ftxui::Box& stencil = screen.stencil;
        const int top = box_.y_min + frame.box.y_min;     // Borde superior del bloque.
        const int bottom = box_.y_min + frame.box.y_max;  // Borde inferior del bloque.
        const int row = std::max(top, stencil.y_min);
        if (row > bottom - 1 || row > stencil.y_max) {
            return; // Ya no queda a la vista ninguna fila de contenido.
        }
        const int left = box_.x_min + frame.box.x_min;
        const int right = box_.x_min + frame.box.x_max;
        const std::optional<ButtonLabels> labels =
            fitting_labels(right - left + 1, frame.title_width,
                           frame.number == view_.copied_block_);
        if (!labels.has_value()) {
            return;
        }
        int x = right - labels_width(*labels);
        // Si la celda de la izquierda es la mitad de un carácter ancho que
        // los botones taparían, se cambia por un espacio.
        if (x - 1 >= stencil.x_min && x - 1 <= stencil.x_max &&
            ftxui::string_width(screen.CellAt(x - 1, row).character) == 2) {
            screen.CellAt(x - 1, row).character = " ";
        }
        x = put(screen, x, row, labels->copy);
        hit(screen, x - ftxui::string_width(std::string{labels->copy}), x - 1, row, frame.number,
            BlockAction::Copy);
        x = put(screen, x, row, " ", false);
        const int save_begin = x;
        x = put(screen, x, row, labels->save);
        hit(screen, save_begin, x - 1, row, frame.number, BlockAction::Save);
    }

    /// Anota el botón de las columnas begin a end (lo que se ve de él).
    void hit(const ftxui::Screen& screen, int begin, int end, int y, int block,
             BlockAction action) const {
        const ftxui::Box box =
            ftxui::Box::Intersection(ftxui::Box{begin, end, y, y}, screen.stencil);
        if (hits_ != nullptr && !box.IsEmpty()) {
            hits_->push_back(ButtonHit{box, block, action});
        }
    }

    /// Escribe label desde (x, y), con la tinta notice sobre el fondo que ya
    /// tiene la celda (y la selección si es un botón con el puntero encima).
    /// Devuelve la columna siguiente.
    int put(ftxui::Screen& screen, int x, int y, std::string_view label,
            bool button = true) const {
        const int end = x + ftxui::string_width(std::string{label}) - 1;
        const bool hovered = button && view_.hover_.has_value() && view_.hover_->second == y &&
                             view_.hover_->first >= x && view_.hover_->first <= end;
        for (const std::string& glyph : ftxui::Utf8ToGlyphs(std::string{label})) {
            if (x >= screen.stencil.x_min && x <= screen.stencil.x_max) {
                ftxui::Cell& cell = screen.CellAt(x, y);
                const ftxui::Color background = cell.background_color;
                cell = ftxui::Cell{};
                cell.character = glyph;
                cell.background_color = background;
                cell.automerge = false;
                view_.palette_.ink_cell(cell, &Theme::notice);
                if (hovered) {
                    view_.palette_.select_cell(cell);
                }
            }
            ++x;
        }
        return x;
    }

    std::shared_ptr<const ftxui::Screen> image_;
    std::vector<CodeFrame> frames_;
    const HistoryView& view_;
    std::vector<ButtonHit>* hits_;
};


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

std::optional<ButtonHit> HistoryView::hit_test(int x, int y) const {
    for (const ButtonHit& hit : hits_) {
        if (hit.box.Contain(x, y)) {
            return hit;
        }
    }
    return std::nullopt;
}

void HistoryView::set_hover(int x, int y) {
    hover_ = std::pair{x, y};
    if (copied_block_ != 0) {
        const std::optional<ButtonHit> hit = hit_test(x, y);
        if (!hit.has_value() || hit->action != BlockAction::Copy || hit->block != copied_block_) {
            copied_block_ = 0; // El puntero salió del botón.
        }
    }
}

void HistoryView::clear_hover() {
    hover_.reset();
    copied_block_ = 0;
}

void HistoryView::show_copied(int block) {
    copied_block_ = block;
    copied_at_ = clock_();
}

ftxui::Element HistoryView::render(const std::vector<Entry>& entries, int width,
                                   const Palette& palette,
                                   const std::vector<int>& first_code_numbers) {
    width = std::max(width, 1);
    palette_ = palette;
    hits_.clear(); // Los de este cuadro los anota Picture::Render.
    if (copied_block_ != 0 && clock_() - copied_at_ >= kCopiedFor) {
        copied_block_ = 0;
    }
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
        rows.push_back(std::make_shared<Picture>(cached.image, cached.code_frames, *this, &hits_));
    }
    return ftxui::vbox(std::move(rows));
}

} // namespace chatbot::cli
