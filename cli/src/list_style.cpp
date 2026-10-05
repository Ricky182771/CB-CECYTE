#include "list_style.h"

#include <utility>

namespace chatbot::cli {

ftxui::Element chosen_mark(bool chosen, const Palette& palette) {
    if (!chosen) {
        return ftxui::text(std::string{kNotChosenMark});
    }
    return ftxui::text(std::string{kChosenMark}) | ftxui::bold |
           palette.ink(&Theme::heading_accent);
}

ftxui::Element list_row(std::string_view label, bool cursor, bool chosen,
                        const Palette& palette) {
    ftxui::Element row = ftxui::hbox({chosen_mark(chosen, palette), ftxui::text(std::string{label})});
    return cursor ? row | palette.selection() : row;
}

ftxui::Element choice_list(const std::vector<std::string>& labels, std::size_t cursor,
                           std::optional<std::size_t> chosen, bool focused,
                           const Palette& palette) {
    ftxui::Elements rows;
    rows.reserve(labels.size());
    for (std::size_t i = 0; i < labels.size(); ++i) {
        ftxui::Element row = list_row(labels[i], focused && i == cursor, chosen == i, palette);
        if (i == cursor) {
            row = row | ftxui::focus;
        }
        rows.push_back(std::move(row));
    }
    return palette.vscroll(ftxui::vbox(std::move(rows))) | ftxui::frame;
}

ftxui::Element field_label(std::string_view text, bool focused, const Palette& palette) {
    ftxui::Element element = ftxui::text(std::string{text});
    return focused ? element | palette.selection() : element;
}

ftxui::Element button_label(std::string_view label, bool focused, const Palette& palette) {
    ftxui::Element element = ftxui::text("[ " + std::string{label} + " ]");
    return focused ? element | palette.selection() : element;
}

} // namespace chatbot::cli
