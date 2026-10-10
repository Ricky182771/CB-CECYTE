#include "sidebar.h"
#include "sidebar_view.h"

#include <catch2/catch_test_macros.hpp>

#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/string.hpp>

#include <string>
#include <vector>

namespace {

using chatbot::cli::CalendarDay;
using chatbot::cli::ConversationSummary;
using chatbot::cli::ListKey;
using chatbot::cli::Sidebar;

constexpr CalendarDay kToday{2026, 10, 4};

ConversationSummary item(std::string id, std::string title, std::string updated_at,
                         bool readable = true) {
    ConversationSummary summary;
    summary.id = std::move(id);
    summary.title = std::move(title);
    summary.updated_at = std::move(updated_at);
    summary.readable = readable;
    return summary;
}

std::vector<ConversationSummary> sample() {
    return {
        item("a", "Recursividad en C++ con ejemplos", "2026-10-04T10:00:00-06:00"),
        item("b", "漢字漢字漢字漢字漢字漢字漢字", "2026-10-04T09:00:00-06:00"),
        item("c", "Emoji 😀😀😀😀😀😀😀😀😀😀", "2026-10-03T09:00:00-06:00"),
        item("roto.json", "roto.json", "", false),
    };
}

/// Dibuja la barra con su borde derecho (el divisor), como en la interfaz.
ftxui::Screen draw(const Sidebar& sidebar, int width, int height, bool focused) {
    const auto rows = sidebar.rows(kToday);
    ftxui::Element element = ftxui::hbox(
        {chatbot::cli::render_sidebar(sidebar, rows, width, height, focused,
                                     chatbot::cli::terminal_palette()),
         chatbot::cli::sidebar_divider(chatbot::cli::terminal_palette())});
    ftxui::Screen screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(width + 1),
                                                 ftxui::Dimension::Fixed(height));
    ftxui::Render(screen, element);
    return screen;
}

/// Texto de cada fila (como Screen::ToString, sin estilos).
std::vector<std::string> rows_of(const ftxui::Screen& screen) {
    std::vector<std::string> rows;
    for (int y = 0; y < screen.dimy(); ++y) {
        std::string row;
        bool previous_wide = false;
        for (int x = 0; x < screen.dimx(); ++x) {
            const std::string& character = screen.CellAt(x, y).character;
            if (!previous_wide) {
                row += character.empty() ? std::string{" "} : character;
            }
            previous_wide = character.size() > 1 && ftxui::string_width(character) == 2;
        }
        rows.push_back(row);
    }
    return rows;
}

/// Fila de la pantalla cuyo texto contiene needle, o -1.
int row_with(const std::vector<std::string>& rows, const std::string& needle) {
    for (std::size_t y = 0; y < rows.size(); ++y) {
        if (rows[y].find(needle) != std::string::npos) {
            return static_cast<int>(y);
        }
    }
    return -1;
}

} // namespace

TEST_CASE("barra: recorte con … sin partir caracteres anchos", "[barra][vista]") {
    using chatbot::cli::fit_width;
    CHECK(fit_width("hola", 4) == "hola");
    CHECK(fit_width("hola mundo", 6) == "hola …");
    CHECK(fit_width("漢字漢字", 5) == "漢字…");
    CHECK(fit_width("漢字漢字", 4) == "漢…"); // "漢字…" mediría 5.
    CHECK(fit_width("😀😀😀", 4) == "😀…");
    CHECK(fit_width("hola", 0).empty());
    for (int width = 1; width <= 12; ++width) {
        CAPTURE(width);
        CHECK(ftxui::string_width(fit_width("漢字 y 😀 emoji largos", width)) <= width);
    }
}

TEST_CASE("barra: dibujo con bordes, grupos y títulos recortados", "[barra][vista]") {
    Sidebar sidebar;
    sidebar.open(sample(), "a");
    const ftxui::Screen screen = draw(sidebar, 24, 12, false);
    const auto rows = rows_of(screen);
    CHECK(rows.front() == "┌ Conversaciones ───────┐");
    CHECK(rows.back() == "└───────────────────────┘");
    for (std::size_t y = 1; y + 1 < rows.size(); ++y) {
        CAPTURE(rows[y]);
        CHECK(rows[y].rfind("│ ", 0) == 0);
        CHECK(rows[y].size() >= 3);
        CHECK(rows[y].substr(rows[y].size() - 3) == "│");
        CHECK(ftxui::string_width(rows[y]) == 25); // Ningún carácter partido.
    }
    // "│ " + 21 columnas de contenido + " " + "│" (el divisor).
    CHECK(rows[1] == "│ + Nueva  (Ctrl+N)     │");
    CHECK(rows[2] == "│ ⚙ Configuración  (F2) │");
    CHECK(rows[3] == "│ Hoy                   │");
    CHECK(rows[4] == "│ ● Recursividad en C+… │"); // La abierta lleva ●.
    CHECK(rows[5] == "│   漢字漢字漢字漢字漢… │"); // 9 anchos (18) + "…" = 19: sin partir.
    CHECK(rows[6] == "│ Ayer                  │");
    CHECK(rows[7] == "│   Emoji 😀😀😀😀😀😀… │");
    CHECK(rows[8] == "│ Sin fecha             │");
    // En las ilegibles se recorta el nombre, no "(ilegible)".
    CHECK(rows[9] == "│   roto.js… (ilegible) │");
    // La tecla de las filas fijas va tenue.
    CHECK(screen.CellAt(2, 2).character == "⚙");
    CHECK_FALSE(screen.CellAt(2, 2).dim);
    CHECK(screen.CellAt(20, 2).dim);
}

TEST_CASE("barra: la ilegible va tenue", "[barra][vista]") {
    Sidebar sidebar;
    sidebar.open(sample(), "");
    const ftxui::Screen screen = draw(sidebar, 40, 12, false);
    const auto rows = rows_of(screen);
    const int y = row_with(rows, "roto.json (ilegible)");
    REQUIRE(y >= 0);
    const int x = static_cast<int>(rows[static_cast<std::size_t>(y)].find("roto"));
    CHECK(screen.CellAt(x, y).dim);
    const int readable = row_with(rows, "Recursividad");
    REQUIRE(readable >= 0);
    CHECK_FALSE(screen.CellAt(4, readable).dim);
    // Los encabezados van en negritas; el relleno, sin estilo.
    const int header = row_with(rows, "Hoy");
    CHECK(screen.CellAt(2, header).bold);
    CHECK_FALSE(screen.CellAt(10, header).bold);
}

TEST_CASE("barra: la fila seleccionada solo se resalta con el foco", "[barra][vista]") {
    Sidebar sidebar;
    sidebar.open(sample(), "a");
    (void)sidebar.handle(ListKey::Down, {}, 10); // De "a" a "b".
    for (const bool focused : {true, false}) {
        CAPTURE(focused);
        const ftxui::Screen screen = draw(sidebar, 30, 12, focused);
        const auto rows = rows_of(screen);
        const int selected = row_with(rows, "漢字");
        REQUIRE(selected >= 0);
        int inverted = 0;
        for (int y = 0; y < screen.dimy(); ++y) {
            for (int x = 0; x < screen.dimx(); ++x) {
                if (screen.CellAt(x, y).inverted) {
                    ++inverted;
                    CHECK(y == selected);
                }
            }
        }
        // Con el foco: todo el ancho de la fila, sin los bordes ni el margen.
        CHECK(inverted == (focused ? 30 - 3 : 0));
        if (focused) {
            CHECK_FALSE(screen.CellAt(0, selected).inverted);
            CHECK(screen.CellAt(2, selected).inverted);
            CHECK(screen.CellAt(28, selected).inverted);
            CHECK_FALSE(screen.CellAt(29, selected).inverted);
        }
    }
}

TEST_CASE("barra: lista vacía y vista desplazada", "[barra][vista]") {
    Sidebar empty;
    empty.open({}, "");
    const auto empty_rows = rows_of(draw(empty, 40, 6, true));
    CHECK(empty_rows[1].find("+ Nueva") != std::string::npos);
    CHECK(empty_rows[2].find("⚙ Configuración") != std::string::npos);
    CHECK(empty_rows[3].find("No hay conversaciones guardadas.") != std::string::npos);

    // Con 3 filas visibles y la última seleccionada, la vista baja.
    Sidebar sidebar;
    sidebar.open(sample(), "");
    (void)sidebar.handle(ListKey::End, {}, 3);
    const auto rows = sidebar.rows(kToday);
    sidebar.fit(rows, static_cast<std::size_t>(chatbot::cli::sidebar_view_height(5)));
    const auto drawn = rows_of(draw(sidebar, 40, 5, true));
    REQUIRE(drawn.size() == 5);
    CHECK(drawn[3].find("roto.json") != std::string::npos);
    CHECK(drawn[1].find("+ Nueva") == std::string::npos);
}

TEST_CASE("barra: un título con saltos de línea ocupa una sola fila", "[barra][vista][varias]") {
    // make_title ya los quita; esto cubre un archivo editado a mano.
    Sidebar sidebar;
    sidebar.open({item("a", "uno\ndos\ntres", "2026-10-04T10:00:00-06:00"),
                  item("b", "otra", "2026-10-04T09:00:00-06:00")},
                 "b");
    const auto rows = rows_of(draw(sidebar, 24, 8, false));
    const int first = row_with(rows, "uno dos tres");
    REQUIRE(first >= 0);
    CHECK(row_with(rows, "otra") == first + 1);
}
