// El componente TextArea (text_area.h): dibujo con ajuste de línea, alto,
// barra, cursor real de la terminal, ratón y teclas, sin terminal.

#include "text_area.h"

#include "no_color_guard.hpp"
#include "paste.h"
#include "symbols_check.hpp"

#include <catch2/catch_test_macros.hpp>

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>

#include <memory>
#include <string>
#include <vector>

namespace {

using chatbot::cli::Palette;
using chatbot::cli::Symbols;
using chatbot::cli::TextArea;

const Palette& unicode_palette() {
    static const Palette palette{chatbot::cli::default_theme(),
                                 chatbot::cli::BackgroundMode::FromTheme, Symbols::Unicode};
    return palette;
}

std::shared_ptr<TextArea> area(const Palette& palette, int max_rows = 8,
                               chatbot::cli::EnterKey enter = chatbot::cli::EnterKey::Submit) {
    chatbot::cli::TextAreaOption option;
    option.palette = &palette;
    option.max_rows = max_rows;
    option.enter = enter;
    option.placeholder = [](int columns) {
        return columns >= 20 ? std::string{"Escribe tu mensaje aquí"} : std::string{"Mensaje"};
    };
    std::shared_ptr<TextArea> field = chatbot::cli::make_text_area(std::move(option));
    field->TakeFocus();
    return field;
}

/// Dibuja el campo solo, como en la caja: el alto lo decide el campo (vbox
/// con un relleno debajo para que no se estire).
ftxui::Screen draw(const std::shared_ptr<TextArea>& field, int width, int height = 12) {
    ftxui::Screen screen =
        ftxui::Screen::Create(ftxui::Dimension::Fixed(width), ftxui::Dimension::Fixed(height));
    ftxui::Render(screen, ftxui::vbox({field->Render(), ftxui::filler()}));
    return screen;
}

/// Texto de la fila y (las celdas vacías como espacio; la segunda celda de
/// un glifo ancho, nada).
std::string row_text(const ftxui::Screen& screen, int y) {
    std::string text;
    for (int x = 0; x < screen.dimx(); ++x) {
        text += screen.CellAt(x, y).character;
    }
    return text;
}

/// Filas que el campo ocupa en la pantalla (hasta la primera vacía).
std::vector<std::string> rows_drawn(const ftxui::Screen& screen) {
    std::vector<std::string> out;
    for (int y = 0; y < screen.dimy(); ++y) {
        std::string row = row_text(screen, y);
        while (!row.empty() && row.back() == ' ') {
            row.pop_back();
        }
        out.push_back(row);
    }
    while (!out.empty() && out.back().empty()) {
        out.pop_back();
    }
    return out;
}

ftxui::Event mouse(int x, int y, ftxui::Mouse::Button button,
                   ftxui::Mouse::Motion motion = ftxui::Mouse::Pressed) {
    ftxui::Mouse m;
    m.x = x;
    m.y = y;
    m.button = button;
    m.motion = motion;
    return ftxui::Event::Mouse("", m);
}

const std::string kParagraph =
    "Hoy aprendimos que la fotosíntesis convierte la luz del sol en energía química, "
    "y que las plantas liberan oxígeno al aire que respiramos.";

} // namespace

TEST_CASE("TextArea: un párrafo largo se ve completo en varias filas", "[text_area]") {
    const auto field = area(unicode_palette(), 20);
    field->set_text(kParagraph);
    const ftxui::Screen screen = draw(field, 20, 20);
    const std::vector<std::string> rows = rows_drawn(screen);
    REQUIRE(rows.size() > 1);
    // Unidas (cada fila con sus espacios), son el texto completo: nada cortado.
    std::string joined;
    for (int y = 0; y < static_cast<int>(rows.size()); ++y) {
        std::string row = row_text(screen, y);
        // Lo que dibuja el campo de esa fila, sin el relleno de la derecha.
        const chatbot::cli::TextRow& model = (*field->edit().rows(20))[static_cast<std::size_t>(y)];
        joined += kParagraph.substr(model.start, model.end - model.start);
        // La última columna es del cursor; solo un espacio colgado la ocupa.
        CHECK(model.width <= (model.hanging ? 20 : 19));
        CHECK(row.rfind(kParagraph.substr(model.start, model.end - model.start), 0) == 0);
        // Ninguna palabra partida: cada fila cortada termina en espacio, y
        // ninguna empieza con espacio.
        INFO(rows[static_cast<std::size_t>(y)]);
        if (model.ends == chatbot::cli::RowBreak::Soft) {
            CHECK(kParagraph[model.end - 1] == ' ');
        }
        CHECK(kParagraph[model.start] != ' ');
    }
    CHECK(joined == kParagraph);
}

TEST_CASE("TextArea: el espacio que ya no cabe ocupa la columna reservada", "[text_area]") {
    // Ancho 5 (útil 4): "abcd " | "efgh". El cursor real nunca queda después
    // del espacio colgado: Fin y el clic lo dejan en su celda.
    const auto field = area(unicode_palette(), 4);
    field->set_text("abcd efgh", 0);
    ftxui::Screen screen = draw(field, 5, 4);
    CHECK(rows_drawn(screen) == std::vector<std::string>{"abcd", "efgh"});
    REQUIRE(field->OnEvent(ftxui::Event::End));
    screen = draw(field, 5, 4);
    CHECK(field->cursor() == 4);
    CHECK(screen.cursor().x == 4);
    CHECK(screen.cursor().y == 0);
    REQUIRE(field->OnEvent(ftxui::Event::ArrowRight)); // Después del espacio: abajo.
    screen = draw(field, 5, 4);
    CHECK(screen.cursor().x == 0);
    CHECK(screen.cursor().y == 1);
    REQUIRE(field->OnEvent(mouse(4, 0, ftxui::Mouse::Left)));
    CHECK(field->cursor() == 4);
}

TEST_CASE("TextArea: glifos anchos y combinantes no se cortan", "[text_area]") {
    const auto field = area(unicode_palette(), 10);
    // Ancho 6 (útil 5): "abc" no deja lugar al 中 en la última celda.
    field->set_text("abcd中文e\xCC\x81");
    const ftxui::Screen screen = draw(field, 6, 6);
    CHECK(rows_drawn(screen).at(0) == "abcd");
    CHECK(screen.CellAt(0, 1).character == "中");
    CHECK(screen.CellAt(1, 1).character.empty());
    CHECK(screen.CellAt(2, 1).character == "文");
    CHECK(screen.CellAt(4, 1).character == "e\xCC\x81");
}

TEST_CASE("TextArea: \\t, controles e inválidos se ven", "[text_area]") {
    const auto field = area(unicode_palette(), 10);
    field->set_text("a\tb\x1B" "c\xFF");
    const ftxui::Screen screen = draw(field, 20, 3);
    CHECK(row_text(screen, 0).rfind("a    b\xEF\xBF\xBD" "c\xEF\xBF\xBD", 0) == 0);
}

TEST_CASE("TextArea: el alto crece con las filas hasta el máximo y luego hay barra",
          "[text_area]") {
    const auto field = area(unicode_palette(), 3);
    field->set_text("uno");
    ftxui::Screen screen = draw(field, 12, 10);
    CHECK(rows_drawn(screen).size() == 1);

    field->set_text("uno dos tres cuatro"); // Ancho útil 11: dos filas.
    screen = draw(field, 12, 10);
    CHECK(rows_drawn(screen) == std::vector<std::string>{"uno dos", "tres cuatro"});
    CHECK_FALSE(field->scrollable());

    field->set_text("uno dos tres cuatro cinco seis siete ocho nueve diez");
    screen = draw(field, 12, 10);
    REQUIRE(field->scrollable());
    // Tres filas: el máximo. La barra en la última columna y el texto con una
    // columna menos; el cursor al final, así que se ven las últimas filas.
    const std::vector<std::string> rows = rows_drawn(screen);
    REQUIRE(rows.size() == 3);
    CHECK(row_text(screen, 2).find("diez") != std::string::npos);
    bool has_bar = false;
    for (int y = 0; y < 3; ++y) {
        const std::string& cell = screen.CellAt(11, y).character;
        has_bar = has_bar || cell == "┃" || cell == "╹" || cell == "╻";
    }
    CHECK(has_bar);
    CHECK(field->row_count() > 3);
}

TEST_CASE("TextArea: la barra no oscila cuando el texto cabe justo", "[text_area]") {
    // Con el ancho completo cabe en el máximo: sin barra. Con una columna
    // menos ya no cabría, pero solo se mide con el ancho completo.
    const auto field = area(unicode_palette(), 2);
    field->set_text("abcdefghij"); // Ancho 11: útil 10, una fila; ancho 10: dos.
    for (int i = 0; i < 3; ++i) {
        const ftxui::Screen screen = draw(field, 11, 4);
        CHECK_FALSE(field->scrollable());
        CHECK(rows_drawn(screen) == std::vector<std::string>{"abcdefghij"});
    }
    field->set_text("abcdefghij abcdefghij abcdefghij"); // Tres filas con 11: barra.
    for (int i = 0; i < 3; ++i) {
        const ftxui::Screen screen = draw(field, 11, 4);
        CHECK(field->scrollable());
        CHECK(rows_drawn(screen).size() == 2);
    }
}

TEST_CASE("TextArea: el pegado como Event::Character inserta todo de una vez",
          "[text_area]") {
    int changes = 0;
    int enters = 0;
    chatbot::cli::TextAreaOption option;
    option.palette = &unicode_palette();
    option.max_rows = 8;
    option.enter = chatbot::cli::EnterKey::Submit;
    option.on_change = [&changes] { ++changes; };
    option.on_enter = [&enters] { ++enters; };
    const auto field = chatbot::cli::make_text_area(std::move(option));
    field->TakeFocus();
    field->set_text("antes  después", 6);
    CHECK(changes == 0); // set_text no avisa.

    const auto paste =
        chatbot::cli::prepare_paste("uno\r\ndos\n", chatbot::cli::PasteTarget::MultiLine);
    REQUIRE(paste.has_value());
    REQUIRE(field->OnEvent(ftxui::Event::Character(paste->text)));
    CHECK(field->text() == "antes uno\ndos\n después");
    CHECK(field->cursor() == 6 + 8);
    CHECK(changes == 1);
    CHECK(enters == 0);
}

TEST_CASE("TextArea: Enter según la instancia", "[text_area]") {
    int enters = 0;
    SECTION("Submit: llama a on_enter sin tocar el texto") {
        const auto field = area(unicode_palette());
        field->set_on_enter([&enters, &field] {
            ++enters;
            field->set_text(""); // Como send(): vacía la caja.
        });
        field->set_text("hola");
        REQUIRE(field->OnEvent(ftxui::Event::Return));
        CHECK(enters == 1);
        CHECK(field->text().empty());
    }
    SECTION("Newline: inserta el salto") {
        const auto field = area(unicode_palette(), 0, chatbot::cli::EnterKey::Newline);
        field->set_text("ab", 1);
        REQUIRE(field->OnEvent(ftxui::Event::Return));
        CHECK(field->text() == "a\nb");
        CHECK(field->cursor() == 2);
    }
}

TEST_CASE("TextArea: Tab y Shift+Tab nunca insertan", "[text_area]") {
    const auto field = area(unicode_palette());
    field->set_text("ab");
    CHECK_FALSE(field->OnEvent(ftxui::Event::Tab));
    CHECK_FALSE(field->OnEvent(ftxui::Event::TabReverse));
    CHECK(field->text() == "ab");
}

TEST_CASE("TextArea: sin el foco no recibe teclas", "[text_area]") {
    const auto field = area(unicode_palette());
    const auto other = ftxui::Input(std::string{});
    const auto container = ftxui::Container::Vertical({field, other});
    other->TakeFocus();
    REQUIRE_FALSE(field->Focused());
    CHECK_FALSE(field->OnEvent(ftxui::Event::Character("x")));
    CHECK(field->text().empty());
}

TEST_CASE("TextArea: set_text deja el cursor al final y visible", "[text_area]") {
    const auto field = area(unicode_palette(), 3);
    std::string text;
    for (int i = 1; i <= 20; ++i) {
        text += "línea " + std::to_string(i) + "\n";
    }
    text += "última";
    field->set_text("x", 0);
    (void)draw(field, 20, 10);
    field->set_text(text);
    CHECK(field->cursor() == text.size());
    const ftxui::Screen screen = draw(field, 20, 10);
    CHECK(row_text(screen, 2).rfind("última", 0) == 0);
    // El cursor real de la terminal, después de "última" (6 glifos).
    CHECK(screen.cursor().x == 6);
    CHECK(screen.cursor().y == 2);
    CHECK(screen.cursor().shape == ftxui::Screen::Cursor::BarBlinking);
}

TEST_CASE("TextArea: el cursor real en un corte suave según la afinidad", "[text_area]") {
    const auto field = area(unicode_palette(), 5);
    field->set_text("hola mundo feliz", 0); // Ancho 12: "hola mundo " | "feliz".
    (void)draw(field, 12, 5);
    REQUIRE(field->OnEvent(ftxui::Event::End));
    ftxui::Screen screen = draw(field, 12, 5);
    CHECK(screen.cursor().x == 11); // En la columna reservada de la fila 0.
    CHECK(screen.cursor().y == 0);
    REQUIRE(field->OnEvent(ftxui::Event::Home));
    REQUIRE(field->OnEvent(ftxui::Event::ArrowDown));
    REQUIRE(field->OnEvent(ftxui::Event::Home));
    screen = draw(field, 12, 5);
    CHECK(screen.cursor().x == 0);
    CHECK(screen.cursor().y == 1);
}

TEST_CASE("TextArea: Insert cambia a sobrescribir con cursor de bloque", "[text_area]") {
    const auto field = area(unicode_palette());
    field->set_text("abc", 0);
    REQUIRE(field->OnEvent(ftxui::Event::Insert));
    REQUIRE(field->OnEvent(ftxui::Event::Character("x")));
    CHECK(field->text() == "xbc");
    const ftxui::Screen screen = draw(field, 20, 3);
    CHECK(screen.cursor().shape == ftxui::Screen::Cursor::BlockBlinking);
}

TEST_CASE("TextArea: clic coloca el cursor y toma el foco", "[text_area]") {
    const auto field = area(unicode_palette(), 5);
    const auto other = ftxui::Input(std::string{});
    const auto container = ftxui::Container::Vertical({field, other});
    other->TakeFocus();
    field->set_text("hola mundo feliz\nab中cd");
    // Ancho 12: "hola mundo " | "feliz" | "ab中cd".
    (void)draw(field, 12, 6);
    SECTION("En un glifo: antes de él") {
        REQUIRE(field->OnEvent(mouse(2, 0, ftxui::Mouse::Left)));
        CHECK(field->Focused());
        CHECK(field->cursor() == 2);
    }
    SECTION("Después del final de una fila cortada: al final, arriba") {
        REQUIRE(field->OnEvent(mouse(11, 0, ftxui::Mouse::Left)));
        CHECK(field->cursor() == 11);
        CHECK(field->edit().cursor().affinity == chatbot::cli::Affinity::Up);
        const ftxui::Screen screen = draw(field, 12, 6);
        CHECK(screen.cursor().y == 0);
        CHECK(screen.cursor().x == 11);
    }
    SECTION("Mitad izquierda de un glifo ancho: antes; derecha: después") {
        REQUIRE(field->OnEvent(mouse(2, 2, ftxui::Mouse::Left)));
        CHECK(field->cursor() == 19); // "hola mundo feliz\n" + "ab".
        REQUIRE(field->OnEvent(mouse(3, 2, ftxui::Mouse::Left)));
        CHECK(field->cursor() == 22);
    }
    SECTION("Fuera del campo no hace nada") {
        CHECK_FALSE(field->OnEvent(mouse(1, 5, ftxui::Mouse::Left))); // El campo mide 3 filas.
        CHECK_FALSE(field->Focused());
    }
}

TEST_CASE("TextArea: clic debajo de la última fila va a la última fila", "[text_area]") {
    // Como en F2: el campo mide más filas (min_rows) que su texto.
    chatbot::cli::TextAreaOption option;
    option.palette = &unicode_palette();
    option.min_rows = 5;
    option.max_rows = 5;
    const auto field = chatbot::cli::make_text_area(std::move(option));
    field->TakeFocus();
    field->set_text("hola\nab", 0);
    (void)draw(field, 20, 6);
    REQUIRE(field->OnEvent(mouse(1, 4, ftxui::Mouse::Left)));
    CHECK(field->cursor() == 6); // Después de "a", en la columna del clic.
    REQUIRE(field->OnEvent(mouse(15, 4, ftxui::Mouse::Left)));
    CHECK(field->cursor() == 7); // Más allá del final: al final.
}

TEST_CASE("TextArea: la rueda mueve la vista, no el cursor", "[text_area]") {
    const auto field = area(unicode_palette(), 3);
    std::string text;
    for (int i = 1; i <= 10; ++i) {
        text += "fila " + std::to_string(i) + (i < 10 ? "\n" : "");
    }
    field->set_text(text);
    ftxui::Screen screen = draw(field, 20, 6);
    REQUIRE(field->scrollable());
    CHECK(row_text(screen, 2).rfind("fila 10", 0) == 0);
    REQUIRE(field->OnEvent(mouse(3, 1, ftxui::Mouse::WheelUp)));
    CHECK(field->cursor() == text.size());
    screen = draw(field, 20, 6);
    CHECK(row_text(screen, 0).rfind("fila 5", 0) == 0);
    // El cursor quedó fuera de la vista: sin cursor real.
    CHECK(screen.cursor().shape == ftxui::Screen::Cursor::Hidden);
    // Escribir regresa la vista al cursor.
    REQUIRE(field->OnEvent(ftxui::Event::Character("!")));
    screen = draw(field, 20, 6);
    CHECK(row_text(screen, 2).rfind("fila 10!", 0) == 0);
    // Sin barra, la rueda no es suya.
    field->set_text("corto");
    (void)draw(field, 20, 6);
    CHECK_FALSE(field->OnEvent(mouse(1, 0, ftxui::Mouse::WheelDown)));
}

namespace {

/// Cuenta las pasadas del layout (ComputeRequirement) del elemento que envuelve.
class PassCounter : public ftxui::Node {
public:
    PassCounter(ftxui::Element child, int& passes)
        : ftxui::Node(ftxui::Elements{std::move(child)}), passes_(passes) {}
    void ComputeRequirement() override {
        ++passes_;
        ftxui::Node::ComputeRequirement();
    }
    void SetBox(ftxui::Box box) override {
        ftxui::Node::SetBox(box);
        children_[0]->SetBox(box);
    }

private:
    int& passes_;
};

} // namespace

TEST_CASE("TextArea: el layout se asienta aunque el campo reciba menos filas de las que pide",
          "[text_area]") {
    // Como F2 en una terminal baja: el campo pide 5 filas (o sus filas, hasta
    // el máximo) y el padre le da 2. Ni pasadas de más en cada cuadro ni el
    // cursor real en la esquina cuando la rueda lo dejó fuera.
    for (const int max_rows : {0, 8}) {
        INFO("max_rows " << max_rows);
        chatbot::cli::TextAreaOption option;
        option.palette = &unicode_palette();
        option.min_rows = 5;
        option.max_rows = max_rows;
        const auto field = chatbot::cli::make_text_area(std::move(option));
        field->TakeFocus();
        std::string text;
        for (int i = 1; i <= 12; ++i) {
            text += "fila " + std::to_string(i) + "\n";
        }
        field->set_text(text);
        const auto frame = [&field](int& passes) {
            ftxui::Screen screen =
                ftxui::Screen::Create(ftxui::Dimension::Fixed(20), ftxui::Dimension::Fixed(3));
            ftxui::Render(screen,
                          ftxui::vbox({std::make_shared<PassCounter>(field->Render(), passes),
                                       ftxui::text("[ Guardar ]")}));
            return screen;
        };
        int passes = 0;
        (void)frame(passes); // El primero puede necesitar más (ancho y alto nuevos).
        for (int i = 0; i < 3; ++i) {
            passes = 0;
            const ftxui::Screen screen = frame(passes);
            CHECK(passes == 1);
            if (max_rows == 0) { // Flex (F2): se encoge y lo de abajo se ve.
                CHECK(row_text(screen, 2).rfind("[ Guardar ]", 0) == 0);
            }
        }
        REQUIRE(field->scrollable());
        REQUIRE(field->OnEvent(mouse(2, 0, ftxui::Mouse::WheelUp)));
        REQUIRE(field->OnEvent(mouse(2, 0, ftxui::Mouse::WheelUp)));
        passes = 0;
        const ftxui::Screen screen = frame(passes);
        CHECK(passes == 1);
        CHECK(screen.cursor().shape == ftxui::Screen::Cursor::Hidden);
    }
}

TEST_CASE("TextArea: con una fila de más que la vista ya hay barra", "[text_area]") {
    const auto field = area(unicode_palette(), 3);
    field->set_text("uno\ndos\ntres\ncuatro"); // 4 filas en un campo de 3.
    const ftxui::Screen screen = draw(field, 12, 5);
    REQUIRE(field->scrollable());
    bool has_bar = false;
    for (int y = 0; y < 3; ++y) {
        const std::string& cell = screen.CellAt(11, y).character;
        has_bar = has_bar || cell == "┃" || cell == "╹" || cell == "╻";
    }
    CHECK(has_bar);
}

TEST_CASE("TextArea: un cambio de ancho conserva el cursor en su lugar", "[text_area]") {
    const auto field = area(unicode_palette(), 4);
    field->set_text(kParagraph, kParagraph.find("energía"));
    for (const int width : {60, 30, 15, 40}) {
        const ftxui::Screen screen = draw(field, width, 8);
        INFO("ancho " << width);
        CHECK(field->cursor() == kParagraph.find("energía"));
        // El cursor real está en la "e" de "energía".
        CHECK(screen.CellAt(screen.cursor().x, screen.cursor().y).character == "e");
        CHECK(screen.CellAt(screen.cursor().x + 1, screen.cursor().y).character == "n");
    }
}

TEST_CASE("TextArea: placeholder atenuado, corto en pantallas angostas", "[text_area]") {
    const chatbot_test::NoColorGuard environment(nullptr);
    const auto field = area(unicode_palette());
    ftxui::Screen screen = draw(field, 30, 2);
    CHECK(row_text(screen, 0).rfind("Escribe tu mensaje aquí", 0) == 0);
    CHECK(screen.CellAt(0, 0).foreground_color ==
          chatbot::cli::to_ftxui(unicode_palette().theme().input_placeholder));
    CHECK(screen.cursor().x == 0);
    screen = draw(field, 12, 2);
    CHECK(row_text(screen, 0).rfind("Mensaje", 0) == 0);
}

TEST_CASE("TextArea: modo Sencillos, la barra con los respaldos", "[text_area][simbolos]") {
    const auto draw_with = [](const Palette& palette) {
        const auto field = area(palette, 3);
        field->set_text("uno dos tres cuatro cinco seis siete ocho nueve diez once doce", 0);
        return draw(field, 12, 4);
    };
    const ftxui::Screen unicode = draw_with(unicode_palette());
    REQUIRE(chatbot_test::table_glyphs_in(unicode) > 0); // La barra.
    for (const Palette& palette : chatbot_test::palettes_with(Symbols::Simple)) {
        const Palette same{palette.theme(), palette.background(), Symbols::Unicode};
        chatbot_test::check_simple_matches(draw_with(same), draw_with(palette));
    }
}
