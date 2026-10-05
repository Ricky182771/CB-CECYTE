#include "title_bar.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/string.hpp>

#include <string>

namespace {

std::string draw(std::string_view title, int width) {
    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(width),
                                        ftxui::Dimension::Fixed(1));
    ftxui::Render(screen, chatbot::cli::title_bar(title, width, chatbot::cli::terminal_palette()));
    std::string text;
    for (int x = 0; x < width; ++x) {
        const auto& character = screen.CellAt(x, 0).character;
        if (character.empty() && x > 0 &&
            ftxui::string_width(screen.CellAt(x - 1, 0).character) == 2) {
            continue;
        }
        text += character.empty() ? " " : character;
    }
    return text;
}

} // namespace

TEST_CASE("título: prioridad sobre la ayuda a 40, 60 y 120 columnas", "[titulo]") {
    const int width = GENERATE(40, 60, 120);
    const std::string title = "Chatbot CECyTE — m — Conversación";
    const std::string row = draw(title, width);
    CHECK(row.starts_with(title));
    CHECK(ftxui::string_width(row) == width);
    if (width == 120) {
        CHECK(row.find("Ctrl+B barra · Ctrl+O conversaciones") != std::string::npos);
    } else if (width == 60) {
        CHECK(row.find("Ctrl+B · Ctrl+O") != std::string::npos);
        CHECK(row.find("barra") == std::string::npos);
    } else {
        CHECK(row.find("Ctrl+B") == std::string::npos);
    }
}

TEST_CASE("título: solo se recorta sin ayuda y respeta UTF-8", "[titulo]") {
    const int width = GENERATE(40, 60, 120);
    std::string title = "Chatbot CECyTE — modelo — ";
    for (int i = 0; i < 100; ++i) {
        title += "🙂é";
    }
    const std::string row = draw(title, width);
    CHECK(row.find("Ctrl+B") == std::string::npos);
    CHECK(row.find("…") != std::string::npos);
    CHECK(row.find("�") == std::string::npos);
    CHECK(ftxui::string_width(row) == width);
}
