#include "command_menu_view.h"

#include "sidebar_view.h"

#include <ftxui/screen/string.hpp>

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>

namespace chatbot::cli {

namespace {

constexpr std::string_view kIndent = "  ";
constexpr std::string_view kGap = "  ";

/// "/buscar <consulta>", o solo el nombre si no lleva argumentos.
std::string usage_of(const CommandInfo& command) {
    std::string usage{command.name};
    if (!command.arguments.empty()) {
        usage += " ";
        usage += command.arguments;
    }
    return usage;
}

std::string spaces(int count) { return std::string(static_cast<std::size_t>(std::max(count, 0)), ' '); }

/// text recortado a width columnas y rellenado con espacios hasta width.
std::string padded(std::string_view text, int width) {
    std::string out = fit_width(text, width);
    return out + spaces(width - ftxui::string_width(out));
}

} // namespace

ftxui::Element render_command_menu(const CommandMenu& menu,
                                   const std::vector<CommandAvailability>& availability,
                                   int width, const Palette& palette,
                                   std::vector<ftxui::Box>* row_boxes) {
    width = std::max(width, 1);
    const int indent = ftxui::string_width(kIndent);
    const int gap = ftxui::string_width(kGap);
    // Alineado al más largo del registro: las columnas no saltan al filtrar.
    int usage_width = 0;
    for (const CommandInfo& command : kCommands) {
        usage_width = std::max(usage_width, ftxui::string_width(usage_of(command)));
    }
    const int description_width = width - indent - usage_width - gap;
    const bool show_description = description_width >= kMenuMinDescription;
    const int left_width = show_description ? usage_width : width - indent;

    ftxui::Elements lines;
    lines.push_back(ftxui::separator() | palette.ink(&Theme::border));
    const std::vector<std::size_t>& matches = menu.matches();
    if (row_boxes != nullptr) {
        row_boxes->assign(matches.size(), ftxui::Box{0, -1, 0, -1});
    }
    if (matches.empty()) {
        lines.push_back(ftxui::text(padded(std::string{kIndent} +
                                               std::string{CommandMenu::kNoMatches},
                                           width)) |
                        palette.ink(&Theme::notice));
        return ftxui::vbox(std::move(lines));
    }
    for (std::size_t row = 0; row < matches.size(); ++row) {
        const CommandInfo& command = kCommands[matches[row]];
        const CommandAvailability state =
            matches[row] < availability.size() ? availability[matches[row]] : CommandAvailability{};
        std::string line = std::string{kIndent} + padded(usage_of(command), left_width);
        if (show_description) {
            line += std::string{kGap} +
                    padded(state.available ? command.description : state.note, description_width);
        }
        line += spaces(width - ftxui::string_width(line));
        ftxui::Element element = ftxui::text(std::move(line));
        if (!state.available) {
            element = element | palette.ink(&Theme::notice);
        }
        if (row == menu.cursor()) {
            element = element | palette.selection();
        }
        if (row_boxes != nullptr) {
            element = element | ftxui::reflect((*row_boxes)[row]);
        }
        lines.push_back(std::move(element));
    }
    return ftxui::vbox(std::move(lines));
}

} // namespace chatbot::cli
