#include "title_bar.h"

#include "markdown.h"

#include <ftxui/screen/string.hpp>

#include <algorithm>
#include <string>

namespace chatbot::cli {

ftxui::Element title_bar(std::string_view title, int width, const Palette& palette) {
    width = std::max(width, 1);
    std::string clean = md::sanitize(title);
    std::replace(clean.begin(), clean.end(), '\n', ' ');
    const int title_width = ftxui::string_width(clean);
    std::string_view help = "Ctrl+B barra · Ctrl+O conversaciones";
    if (title_width + 1 + ftxui::string_width(help) > width) {
        help = "Ctrl+B · Ctrl+O";
    }
    if (title_width + 1 + ftxui::string_width(help) > width) {
        help = {};
    }
    if (title_width > width) {
        std::string prefix;
        int used = 0;
        for (const auto& glyph : ftxui::Utf8ToGlyphs(clean)) {
            const int glyph_width = ftxui::string_width(glyph);
            if (used + glyph_width > width - 1) {
                break;
            }
            prefix += glyph;
            used += glyph_width;
        }
        clean = prefix + "…";
    }
    return ftxui::hbox({ftxui::text(clean) | ftxui::bold, ftxui::filler(),
                        ftxui::text(help) | palette.ink(&Theme::notice)});
}

} // namespace chatbot::cli
