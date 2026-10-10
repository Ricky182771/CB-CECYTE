// La caja de la conversación con varias líneas: lo que main.cpp supone del
// ftxui::Input de FTXUI v7.0.3 (si cambia al actualizar FTXUI, falla aquí).

#include "input_edit.h"

#include <catch2/catch_test_macros.hpp>

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>

#include <string>

namespace {

/// "línea 1\nlínea 2\n…\nlínea count".
std::string numbered_lines(int count) {
    std::string text;
    for (int i = 1; i <= count; ++i) {
        text += (i > 1 ? "\n" : "") + std::string{"línea "} + std::to_string(i);
    }
    return text;
}

/// Texto de la fila y de la pantalla (las celdas vacías como espacio).
std::string row_text(const ftxui::Screen& screen, int y) {
    std::string text;
    for (int x = 0; x < screen.dimx(); ++x) {
        const std::string& character = screen.CellAt(x, y).character;
        text += character.empty() ? " " : character;
    }
    return text;
}

/// Dibuja la caja como main.cpp: con el alto de input_height.
ftxui::Screen draw(const ftxui::Component& input, const std::string& text, int terminal_rows) {
    const int height = chatbot::cli::input_height(text, terminal_rows);
    ftxui::Screen screen =
        ftxui::Screen::Create(ftxui::Dimension::Fixed(30), ftxui::Dimension::Fixed(height));
    ftxui::Render(screen, input->Render() | ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, height));
    return screen;
}

} // namespace

TEST_CASE("caja de varias líneas: el frame sigue la línea del cursor", "[entrada][varias]") {
    std::string text = numbered_lines(20);
    int cursor = static_cast<int>(text.size());
    ftxui::InputOption option;
    option.multiline = false;
    option.cursor_position = &cursor;
    const ftxui::Component input = ftxui::Input(&text, option);
    input->TakeFocus();

    SECTION("Con el cursor al final se ve la última línea") {
        const ftxui::Screen screen = draw(input, text, 40);
        REQUIRE(screen.dimy() == 8);
        CHECK(row_text(screen, 7).rfind("línea 20", 0) == 0);
        CHECK(row_text(screen, 0).rfind("línea 13", 0) == 0);
    }
    SECTION("Con el cursor al principio, la primera") {
        cursor = 0;
        const ftxui::Screen screen = draw(input, text, 40);
        CHECK(row_text(screen, 0).rfind("línea 1 ", 0) == 0);
    }
    SECTION("↑ mueve el cursor a la línea de arriba y la vista lo sigue") {
        cursor = 0;
        for (int i = 0; i < 15; ++i) {
            REQUIRE(input->OnEvent(ftxui::Event::ArrowDown));
        }
        const ftxui::Screen screen = draw(input, text, 40);
        bool visible = false;
        for (int y = 0; y < screen.dimy(); ++y) {
            visible = visible || row_text(screen, y).rfind("línea 16", 0) == 0;
        }
        CHECK(visible);
        REQUIRE(input->OnEvent(ftxui::Event::ArrowUp));
        CHECK(text.substr(0, static_cast<std::size_t>(cursor)).find("línea 16") ==
              std::string::npos);
    }
    SECTION("En una terminal baja, un tercio de su alto") {
        const ftxui::Screen screen = draw(input, text, 12);
        REQUIRE(screen.dimy() == 4);
        CHECK(row_text(screen, 3).rfind("línea 20", 0) == 0);
    }
}
