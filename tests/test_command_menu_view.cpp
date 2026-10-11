#include "command_menu.h"
#include "command_menu_view.h"
#include "commands.h"
#include "theme.h"

#include <catch2/catch_test_macros.hpp>

#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/box.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/string.hpp>

#include <string>
#include <vector>

namespace {

using chatbot::cli::CommandAvailability;
using chatbot::cli::CommandContext;
using chatbot::cli::CommandMenu;

/// Todo disponible.
std::vector<CommandAvailability> all_available() {
    return chatbot::cli::command_availability(CommandContext{true, true, true, true, false});
}

ftxui::Screen draw(const CommandMenu& menu, const std::vector<CommandAvailability>& availability,
                   int width, const chatbot::cli::Palette& palette,
                   std::vector<ftxui::Box>* boxes = nullptr) {
    ftxui::Element element =
        chatbot::cli::render_command_menu(menu, availability, width, palette, boxes);
    ftxui::Screen screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(width),
                                                 ftxui::Dimension::Fit(element));
    ftxui::Render(screen, element);
    return screen;
}

/// Texto de cada fila (sin estilos; las celdas vacías como espacio).
std::vector<std::string> rows_of(const ftxui::Screen& screen) {
    std::vector<std::string> rows;
    for (int y = 0; y < screen.dimy(); ++y) {
        std::string row;
        for (int x = 0; x < screen.dimx(); ++x) {
            const std::string& character = screen.CellAt(x, y).character;
            row += character.empty() ? std::string{" "} : character;
        }
        rows.push_back(row);
    }
    return rows;
}

/// Columna (en celdas) donde empieza needle en la fila, o -1.
int column_of(const ftxui::Screen& screen, int y, const std::string& needle) {
    std::string row;
    std::vector<int> columns; // Columna de cada byte de row.
    for (int x = 0; x < screen.dimx(); ++x) {
        const std::string& character = screen.CellAt(x, y).character;
        for (std::size_t i = 0; i < character.size(); ++i) {
            columns.push_back(x);
        }
        row += character;
    }
    const std::size_t at = row.find(needle);
    return at == std::string::npos ? -1 : columns[at];
}

} // namespace

TEST_CASE("Menú de comandos: columnas alineadas", "[command_menu_view]") {
    CommandMenu menu;
    menu.update("/", true);
    const ftxui::Screen screen = draw(menu, all_available(), 100, chatbot::cli::terminal_palette());
    const std::vector<std::string> rows = rows_of(screen);
    REQUIRE(rows.size() == 5);
    // Arriba, la línea que lo separa del historial.
    CHECK(rows[0].find("────") == 0);
    CHECK(rows[1].find("  /buscar <consulta>") == 0);
    CHECK(rows[2].find("  /copiar [número de bloque]") == 0);
    CHECK(rows[3].find("  /guardar [número de bloque [nombre]]") == 0);
    CHECK(rows[4].find("  /exportar") == 0);
    // La descripción empieza en la misma columna en todas las filas: después
    // del uso más largo y dos espacios.
    const int description = 2 + ftxui::string_width("/guardar [número de bloque [nombre]]") + 2;
    CHECK(column_of(screen, 1, "Busca en la web") == description);
    CHECK(column_of(screen, 2, "Copia un bloque") == description);
    CHECK(column_of(screen, 3, "Guarda un bloque") == description);
    CHECK(column_of(screen, 4, "Exporta la conversación") == description);
    // Sin "> " como prefijo del cursor (la fila 1 lo tiene): dos espacios.
    CHECK(rows[1].rfind("  /", 0) == 0);
}

TEST_CASE("Menú de comandos: el cursor lleva la selección a todo lo ancho", "[command_menu_view]") {
    CommandMenu menu;
    menu.update("/", true);
    (void)menu.handle(chatbot::cli::ListKey::Down); // /copiar
    const ftxui::Screen screen = draw(menu, all_available(), 100, chatbot::cli::terminal_palette());
    // Tema "De la terminal": la selección es invertida.
    CHECK(screen.CellAt(0, 2).inverted);
    CHECK(screen.CellAt(99, 2).inverted);
    CHECK_FALSE(screen.CellAt(0, 1).inverted);
    CHECK_FALSE(screen.CellAt(0, 3).inverted);
}

TEST_CASE("Menú de comandos: los no disponibles muestran su nota, atenuados",
          "[command_menu_view]") {
    CommandMenu menu;
    menu.update("/", true);
    CommandContext context{true, false, false, true, false}; // Sin key ni bloques.
    const ftxui::Screen screen = draw(menu, chatbot::cli::command_availability(context), 100,
                                      chatbot::cli::terminal_palette());
    const std::vector<std::string> rows = rows_of(screen);
    CHECK(rows[1].find("falta la key de búsqueda (F2)") != std::string::npos);
    CHECK(rows[1].find("Busca en la web") == std::string::npos);
    CHECK(rows[2].find("no hay bloques de código") != std::string::npos);
    CHECK(rows[4].find("Exporta la conversación") != std::string::npos);
    // Con el tema "De la terminal", notice es dim. El cursor (fila 1) usa la
    // selección, que lo quita; la fila 2 no tiene cursor.
    CHECK(screen.CellAt(2, 2).dim);
    CHECK_FALSE(screen.CellAt(2, 4).dim);
}

TEST_CASE("Menú de comandos: recorte con … en ancho chico", "[command_menu_view]") {
    CommandMenu menu;
    menu.update("/", true);
    SECTION("La descripción se recorta") {
        const ftxui::Screen screen =
            draw(menu, all_available(), 60, chatbot::cli::terminal_palette());
        const std::vector<std::string> rows = rows_of(screen);
        CHECK(rows[1].find("/buscar <consulta>") != std::string::npos);
        CHECK(rows[1].find("Busca en la") != std::string::npos);
        CHECK(rows[1].find("…") != std::string::npos);
        for (const std::string& row : rows) {
            CHECK(ftxui::string_width(row) == 60);
        }
    }
    SECTION("En pantallas angostas, la descripción se oculta antes que el nombre") {
        const ftxui::Screen screen =
            draw(menu, all_available(), 45, chatbot::cli::terminal_palette());
        const std::vector<std::string> rows = rows_of(screen);
        CHECK(rows[3].find("/guardar [número de bloque [nombre]]") != std::string::npos);
        CHECK(rows[1].find("Busca") == std::string::npos);
    }
    SECTION("Más angosto todavía, el nombre se recorta con …") {
        const ftxui::Screen screen =
            draw(menu, all_available(), 20, chatbot::cli::terminal_palette());
        const std::vector<std::string> rows = rows_of(screen);
        CHECK(rows[3].find("  /guardar [número") == 0);
        CHECK(rows[3].find("…") != std::string::npos);
        CHECK(rows[3].find("nombre") == std::string::npos);
        CHECK(rows[4].find("  /exportar") == 0);
    }
}

TEST_CASE("Menú de comandos: fila de sin coincidencias", "[command_menu_view]") {
    CommandMenu menu;
    menu.update("/xyz", true);
    std::vector<ftxui::Box> boxes{ftxui::Box{}};
    const ftxui::Screen screen =
        draw(menu, all_available(), 80, chatbot::cli::terminal_palette(), &boxes);
    const std::vector<std::string> rows = rows_of(screen);
    REQUIRE(rows.size() == 2);
    CHECK(rows[1].find("  Sin comandos que coincidan · se enviará como mensaje") == 0);
    // Atenuada y sin cursor; ninguna fila elegible para el ratón.
    CHECK(screen.CellAt(2, 1).dim);
    CHECK_FALSE(screen.CellAt(2, 1).inverted);
    CHECK(boxes.empty());
}

TEST_CASE("Menú de comandos: una caja por fila para el ratón", "[command_menu_view]") {
    CommandMenu menu;
    menu.update("/", true);
    std::vector<ftxui::Box> boxes;
    (void)draw(menu, all_available(), 80, chatbot::cli::terminal_palette(), &boxes);
    REQUIRE(boxes.size() == 4);
    for (std::size_t i = 0; i < boxes.size(); ++i) {
        CHECK(boxes[i].y_min == static_cast<int>(i) + 1);
        CHECK(boxes[i].y_max == static_cast<int>(i) + 1);
        CHECK(boxes[i].x_min == 0);
        CHECK(boxes[i].x_max == 79);
    }
}

TEST_CASE("Menú de comandos: colores del tema", "[command_menu_view]") {
    // Con un tema propio, la nota usa el color notice y el cursor la
    // selección del tema (contraste ya revisado en test_theme).
    const chatbot::cli::Palette palette{chatbot::cli::default_theme()};
    CommandMenu menu;
    menu.update("/", true);
    CommandContext context{true, true, false, false, false};
    const ftxui::Screen screen =
        draw(menu, chatbot::cli::command_availability(context), 100, palette);
    CHECK(screen.CellAt(2, 2).foreground_color ==
          chatbot::cli::to_ftxui(palette.theme().notice));
    CHECK(screen.CellAt(2, 1).background_color ==
          chatbot::cli::to_ftxui(palette.theme().selection_bg));
    CHECK(screen.CellAt(0, 0).foreground_color ==
          chatbot::cli::to_ftxui(palette.theme().border));
}
