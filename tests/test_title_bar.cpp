#include "title_bar.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <ftxui/dom/node.hpp>
#include <ftxui/screen/box.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/string.hpp>
#include <ftxui/screen/terminal.hpp>

#include <string>

namespace {

/// Barra dibujada en una fila: el texto y la pantalla.
struct Drawn {
    std::string text;
    ftxui::Screen screen{1, 1};
    ftxui::Box box;
};

Drawn draw_bar(std::string_view title, int width, chatbot::cli::ExportButton button = {true, false},
               const chatbot::cli::Palette& palette = chatbot::cli::terminal_palette()) {
    Drawn drawn;
    drawn.screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(width),
                                         ftxui::Dimension::Fixed(1));
    ftxui::Render(drawn.screen,
                  chatbot::cli::title_bar(title, width, palette, button, drawn.box));
    for (int x = 0; x < width; ++x) {
        const auto& character = drawn.screen.CellAt(x, 0).character;
        if (character.empty() && x > 0 &&
            ftxui::string_width(drawn.screen.CellAt(x - 1, 0).character) == 2) {
            continue;
        }
        drawn.text += character.empty() ? " " : character;
    }
    return drawn;
}

std::string draw(std::string_view title, int width) { return draw_bar(title, width).text; }

} // namespace

TEST_CASE("título: prioridad sobre la ayuda a 40, 60 y 120 columnas", "[titulo]") {
    const int width = GENERATE(40, 60, 120);
    const std::string title = "Chatbot CECyTE — m — Conversación";
    const std::string row = draw(title, width);
    CHECK(row.starts_with(title));
    CHECK(ftxui::string_width(row) == width);
    if (width == 120) {
        CHECK(row.find("Ctrl+B barra · Ctrl+O conversaciones [Exportar]") != std::string::npos);
    } else if (width == 60) {
        CHECK(row.find("Ctrl+B · Ctrl+O [Exportar]") != std::string::npos);
        CHECK(row.find("barra") == std::string::npos);
    } else {
        CHECK(row.find("Ctrl+B") == std::string::npos);
        CHECK(row.find("[Exportar]") == std::string::npos);
    }
}

TEST_CASE("título: primero el título, luego [Exportar] y al final la ayuda", "[titulo]") {
    const std::string title = "Chatbot CECyTE — m — Conversación"; // 33 columnas.
    // 33 + 1 + 10: el botón cabe justo, sin ayuda.
    Drawn bar = draw_bar(title, 44);
    CHECK(bar.text == title + " [Exportar]");
    CHECK(bar.box.x_min == 34);
    CHECK(bar.box.x_max == 43);
    CHECK(bar.box.y_min == 0);
    // Con 59 columnas la ayuda corta aún no cabe junto al botón; con 60, sí.
    CHECK(draw(title, 59).find("Ctrl+B") == std::string::npos);
    CHECK(draw(title, 59).ends_with("[Exportar]"));
    CHECK(draw(title, 60).ends_with("Ctrl+B · Ctrl+O [Exportar]"));
    // Una columna menos: el botón desaparece y el título no se recorta.
    bar = draw_bar(title, 43);
    CHECK(bar.text.starts_with(title));
    CHECK(bar.text.find("[Exportar]") == std::string::npos);
    CHECK(bar.box.IsEmpty());
    CHECK_FALSE(bar.box.Contain(0, 0));
    // El título se recorta solo sin botón.
    CHECK(draw(title, 30).find("…") != std::string::npos);
    CHECK(draw(title, 30).find("[Exportar]") == std::string::npos);
}

TEST_CASE("título: [Exportar] atenuado sin respuestas y con hover si hay", "[titulo]") {
    const std::string title = "Chatbot";
    // "De la terminal": el texto de ejemplo es tenue y la selección, invertida.
    Drawn bar = draw_bar(title, 40, {false, true});
    REQUIRE_FALSE(bar.box.IsEmpty());
    CHECK(bar.screen.CellAt(bar.box.x_min, 0).dim);
    CHECK_FALSE(bar.screen.CellAt(bar.box.x_min, 0).inverted); // Sin acción, sin hover.

    bar = draw_bar(title, 40, {true, false});
    CHECK(bar.screen.CellAt(bar.box.x_min, 0).character == "[");
    CHECK_FALSE(bar.screen.CellAt(bar.box.x_min, 0).inverted);
    CHECK_FALSE(bar.screen.CellAt(bar.box.x_min, 0).dim); // Activo: se distingue.
    bar = draw_bar(title, 40, {true, true});
    for (int x = bar.box.x_min; x <= bar.box.x_max; ++x) {
        CHECK(bar.screen.CellAt(x, 0).inverted);
    }
    CHECK(bar.box.x_max == 39);
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

TEST_CASE("título: [Exportar] activo y atenuado se distinguen en cada tema", "[titulo][tema]") {
    ftxui::Terminal::SetColorSupport(ftxui::Terminal::Color::TrueColor);
    for (const chatbot::cli::Theme& theme : chatbot::cli::themes()) {
        INFO("tema " << theme.id);
        const chatbot::cli::Palette palette(theme);
        const Drawn on = draw_bar("Chatbot", 40, {true, false}, palette);
        const Drawn off = draw_bar("Chatbot", 40, {false, false}, palette);
        const ftxui::Cell& a = on.screen.CellAt(on.box.x_min, 0);
        const ftxui::Cell& b = off.screen.CellAt(off.box.x_min, 0);
        CHECK((a.foreground_color != b.foreground_color || a.dim != b.dim));
    }
}
