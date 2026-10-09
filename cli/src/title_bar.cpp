#include "title_bar.h"

#include "markdown.h"

#include <ftxui/screen/string.hpp>

#include <algorithm>
#include <string>

namespace chatbot::cli {

ftxui::Element title_bar(std::string_view title, int width, const Palette& palette,
                         const ExportButton& button, ftxui::Box& box) {
    width = std::max(width, 1);
    box = ftxui::Box{0, -1, 0, -1}; // Vacía hasta que se dibuje el botón.
    std::string clean = md::sanitize(title);
    std::replace(clean.begin(), clean.end(), '\n', ' ');
    const int title_width = ftxui::string_width(clean);
    const int button_width = ftxui::string_width(std::string{kExportLabel});
    // El botón, con un espacio de separación, solo si cabe junto al título entero.
    const bool show_button = title_width + 1 + button_width <= width;
    const int used = title_width + (show_button ? 1 + button_width : 0);
    std::string_view help = "Ctrl+B barra · Ctrl+O conversaciones";
    if (used + 1 + ftxui::string_width(help) > width) {
        help = "Ctrl+B · Ctrl+O";
    }
    if (used + 1 + ftxui::string_width(help) > width) {
        help = {};
    }
    if (title_width > width) {
        std::string prefix;
        int taken = 0;
        for (const auto& glyph : ftxui::Utf8ToGlyphs(clean)) {
            const int glyph_width = ftxui::string_width(glyph);
            if (taken + glyph_width > width - 1) {
                break;
            }
            prefix += glyph;
            taken += glyph_width;
        }
        clean = prefix + "…";
    }
    ftxui::Elements row{ftxui::text(clean) | ftxui::bold, ftxui::filler(),
                        ftxui::text(std::string{help}) | palette.ink(&Theme::notice)};
    if (show_button) {
        // Activo, en el color del texto (más visible que la ayuda, que va en
        // notice); sin respuestas, atenuado con el del texto de ejemplo de la
        // caja. notice no sirve para distinguirlos: en los dos temas es igual
        // a input_placeholder.
        ftxui::Element label = ftxui::text(std::string{kExportLabel}) |
                               palette.ink(button.enabled ? &Theme::text
                                                          : &Theme::input_placeholder);
        if (button.enabled && button.hovered) {
            label = std::move(label) | palette.selection();
        }
        row.push_back(ftxui::text(help.empty() ? "" : " "));
        row.push_back(std::move(label) | ftxui::reflect(box));
    }
    return ftxui::hbox(std::move(row));
}

} // namespace chatbot::cli
