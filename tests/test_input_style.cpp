#include "input_style.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/color.hpp>
#include <ftxui/screen/screen.hpp>

#include <string>

namespace {

constexpr int kWidth = 30;

/// Dibuja lo que regresa el transform para el contenido dado. Como el Input
/// de FTXUI, el elemento ocupa toda la línea (xflex | frame).
ftxui::Screen draw(const std::string& content, bool hovered, bool focused, bool placeholder) {
    ftxui::Element element = ftxui::text(content) | ftxui::xflex | ftxui::frame;
    element = chatbot::cli::input_transform(std::move(element), hovered, focused, placeholder);
    ftxui::Screen screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(kWidth),
                                                 ftxui::Dimension::Fixed(1));
    ftxui::Render(screen, element);
    return screen;
}

} // namespace

TEST_CASE("caja de entrada: sin invertido ni fondo, con foco o sin él", "[entrada]") {
    const bool focused = GENERATE(true, false);
    const bool hovered = GENERATE(true, false);
    const bool placeholder = GENERATE(true, false);
    CAPTURE(focused, hovered, placeholder);
    const std::string content = placeholder ? "Escribe tu mensaje" : "hola mundo";
    const ftxui::Screen screen = draw(content, hovered, focused, placeholder);
    for (int x = 0; x < kWidth; ++x) {
        CAPTURE(x);
        const ftxui::Cell& cell = screen.CellAt(x, 0);
        CHECK_FALSE(cell.inverted);
        CHECK(cell.background_color == ftxui::Color(ftxui::Color::Default));
        CHECK_FALSE(cell.underlined); // Tampoco subrayado con el ratón encima.
    }
    // El placeholder va en dim; el texto escrito, no.
    for (int x = 0; x < static_cast<int>(content.size()); ++x) {
        CAPTURE(x);
        CHECK(screen.CellAt(x, 0).character == std::string(1, content[static_cast<std::size_t>(x)]));
        CHECK(screen.CellAt(x, 0).dim == placeholder);
    }
}
