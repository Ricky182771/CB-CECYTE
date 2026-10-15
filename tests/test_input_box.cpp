// La caja de la conversación con varias líneas: el TextArea configurado como
// en main.cpp (Enter envía, tope input_max_rows) y lo que main.cpp supone de él.

#include "input_edit.h"
#include "paste.h"
#include "text_area.h"
#include "theme.h"

#include <catch2/catch_test_macros.hpp>

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>

#include <memory>
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

const chatbot::cli::Palette& palette() {
    static const chatbot::cli::Palette value{chatbot::cli::resolve_appearance("", "")};
    return value;
}

/// La caja como en main.cpp.
std::shared_ptr<chatbot::cli::TextArea> box(int* enters = nullptr) {
    chatbot::cli::TextAreaOption option;
    option.palette = &palette();
    option.max_rows = chatbot::cli::kMaxInputLines;
    option.enter = chatbot::cli::EnterKey::Submit;
    option.on_enter = [enters] {
        if (enters != nullptr) {
            ++*enters;
        }
    };
    auto field = chatbot::cli::make_text_area(std::move(option));
    field->TakeFocus();
    return field;
}

/// Dibuja la caja como main.cpp, con el tope de una terminal de terminal_rows
/// filas; la caja decide su alto (debajo, un relleno que no la estira).
ftxui::Screen draw(const std::shared_ptr<chatbot::cli::TextArea>& field, int terminal_rows,
                   int* height = nullptr) {
    field->set_max_rows(chatbot::cli::input_max_rows(terminal_rows));
    ftxui::Screen screen =
        ftxui::Screen::Create(ftxui::Dimension::Fixed(30), ftxui::Dimension::Fixed(terminal_rows));
    ftxui::Box drawn;
    ftxui::Render(screen, ftxui::vbox({field->Render() | ftxui::reflect(drawn), ftxui::filler()}));
    if (height != nullptr) {
        *height = drawn.y_max - drawn.y_min + 1;
    }
    return screen;
}

} // namespace

TEST_CASE("caja de varias líneas: la vista sigue la fila del cursor", "[entrada][varias]") {
    const auto field = box();
    const std::string text = numbered_lines(20);
    field->set_text(text);
    int height = 0;

    SECTION("Con el cursor al final se ve la última línea") {
        const ftxui::Screen screen = draw(field, 40, &height);
        REQUIRE(height == 8);
        CHECK(row_text(screen, 7).rfind("línea 20", 0) == 0);
        CHECK(row_text(screen, 0).rfind("línea 13", 0) == 0);
    }
    SECTION("Con el cursor al principio, la primera") {
        field->set_text(text, 0);
        const ftxui::Screen screen = draw(field, 40);
        CHECK(row_text(screen, 0).rfind("línea 1 ", 0) == 0);
    }
    SECTION("↓ y ↑ mueven el cursor entre filas y la vista lo sigue") {
        field->set_text(text, 0);
        (void)draw(field, 40);
        for (int i = 0; i < 15; ++i) {
            REQUIRE(field->OnEvent(ftxui::Event::ArrowDown));
        }
        const ftxui::Screen screen = draw(field, 40);
        bool visible = false;
        for (int y = 0; y < 8; ++y) {
            visible = visible || row_text(screen, y).rfind("línea 16", 0) == 0;
        }
        CHECK(visible);
        REQUIRE(field->OnEvent(ftxui::Event::ArrowUp));
        CHECK(text.substr(0, field->cursor()).find("línea 16") == std::string::npos);
    }
    SECTION("En una terminal baja, un tercio de su alto") {
        const ftxui::Screen screen = draw(field, 12, &height);
        REQUIRE(height == 4);
        CHECK(row_text(screen, 3).rfind("línea 20", 0) == 0);
    }
    SECTION("Crece con las filas en pantalla, no con las líneas") {
        // Una sola línea lógica que ocupa tres filas a 30 columnas.
        field->set_text("uno dos tres cuatro cinco seis siete ocho nueve diez once doce");
        (void)draw(field, 40, &height);
        CHECK(height == 3);
        field->set_text("");
        (void)draw(field, 40, &height);
        CHECK(height == 1);
    }
}

TEST_CASE("caja de varias líneas: un pegado se inserta de una vez en el cursor",
          "[entrada][varias]") {
    int enters = 0;
    const auto field = box(&enters);
    field->set_text("antes  después", 6);

    const auto paste =
        chatbot::cli::prepare_paste("uno\r\ndos\n", chatbot::cli::PasteTarget::MultiLine);
    REQUIRE(paste.has_value());
    REQUIRE(field->OnEvent(ftxui::Event::Character(paste->text)));
    CHECK(field->text() == "antes uno\ndos\n después");
    CHECK(field->cursor() == 6 + 8);
    CHECK(enters == 0);

    // Por qué main.cpp inserta a mano un "\n" pegado solo (TextArea::insert):
    // Event compara solo el texto, así que para la caja sería Enter.
    CHECK(ftxui::Event::Character("\n") == ftxui::Event::Return);
    REQUIRE(field->OnEvent(ftxui::Event::Character("\n")));
    CHECK(enters == 1);
    field->insert("\n");
    CHECK(field->text() == "antes uno\ndos\n\n después");
}

TEST_CASE("caja de varias líneas: \\ + Enter con el cursor de la caja", "[entrada][varias]") {
    // Lo que hace root en main.cpp antes de que Enter llegue a la caja.
    const auto field = box();
    field->set_text("hola\\ mundo", 5);
    std::string text = field->text();
    int cursor = static_cast<int>(field->cursor());
    REQUIRE(chatbot::cli::backslash_newline(text, cursor));
    field->set_text(std::move(text), static_cast<std::size_t>(cursor));
    CHECK(field->text() == "hola\n mundo");
    CHECK(field->cursor() == 5);
}
