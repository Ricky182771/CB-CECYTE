#include "history_view.h"

#include "markdown_view.h"

#include <ftxui/dom/node.hpp>
#include <ftxui/dom/requirement.hpp>
#include <ftxui/screen/box.hpp>
#include <ftxui/screen/color.hpp>

#include <algorithm>
#include <utility>

namespace chatbot::cli {

namespace {

/// Etiqueta decorada ("Tú:", "(cancelada)"...) en un hbox: dentro del vbox
/// de la entrada se estiraría a todo el ancho, y el estilo con ella.
ftxui::Element label(ftxui::Element element) { return ftxui::hbox({std::move(element)}); }

bool same_entry(const Entry& a, const Entry& b) {
    return a.kind == b.kind && a.in_progress == b.in_progress && a.incomplete == b.incomplete &&
           a.cancelled == b.cancelled && a.text == b.text && a.note == b.note;
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

ftxui::Element HistoryView::entry_element(Cached& cached, const Entry& entry, int width) {
    switch (entry.kind) {
    case EntryKind::User:
        return ftxui::vbox({label(ftxui::text("Tú:") | ftxui::bold), md::render_plain(entry.text, width)});
    case EntryKind::Assistant: {
        ftxui::Elements lines{label(ftxui::text("Asistente:") | ftxui::bold),
                              md::render(document_for(cached, entry.text), width)};
        if (entry.cancelled) {
            lines.push_back(label(ftxui::text("(cancelada)") | ftxui::dim));
        } else if (entry.incomplete) {
            lines.push_back(label(ftxui::text("(respuesta incompleta)") | ftxui::dim));
        } else if (!entry.note.empty()) {
            lines.push_back(md::render_plain(entry.note, width, ftxui::dim));
        }
        return ftxui::vbox(std::move(lines));
    }
    case EntryKind::Error:
        return md::render_plain(entry.text, width, ftxui::color(ftxui::Color::Red));
    case EntryKind::Notice:
        return md::render_plain(entry.text, width, ftxui::dim);
    }
    return ftxui::text("");
}

ftxui::Element HistoryView::render(const std::vector<Entry>& entries, int width) {
    width = std::max(width, 1);
    // Si cambió la conversación, las entradas se comparan por contenido: las
    // iguales se reusan y las demás se vuelven a dibujar.
    cache_.resize(entries.size());
    ftxui::Elements rows;
    rows.reserve(entries.size() * 2);
    for (std::size_t i = 0; i < entries.size(); ++i) {
        Cached& cached = cache_[i];
        const Entry& entry = entries[i];
        if (!cached.image || cached.width != width || !same_entry(cached.drawn, entry)) {
            ftxui::Element element = entry_element(cached, entry, width);
            // El ajuste de líneas es propio (sin flexbox), así que el alto
            // mínimo es el alto final: no hace falta Dimension::Fit.
            element->ComputeRequirement();
            const int height = std::max(element->requirement().min_y, 1);
            auto image = std::make_shared<ftxui::Screen>(width, height);
            ftxui::Render(*image, element);
            cached.image = std::move(image);
            cached.width = width;
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
