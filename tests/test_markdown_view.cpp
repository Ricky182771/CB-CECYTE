#include "markdown.h"
#include "markdown_view.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/string.hpp>

#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace md = chatbot::cli::md;

/// Dibuja el markdown con el ancho dado y el alto que necesite.
ftxui::Screen draw(std::string_view markdown, int width) {
    ftxui::Element element = md::render(md::parse(markdown), width);
    ftxui::Screen screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(width),
                                                 ftxui::Dimension::Fit(element, true));
    ftxui::Render(screen, element);
    return screen;
}

/// Texto de cada fila, sin espacios al final (como Screen::ToString: una
/// celda vacía es un espacio y la celda tras un carácter ancho se salta).
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
        row.erase(row.find_last_not_of(' ') + 1);
        rows.push_back(row);
    }
    return rows;
}

std::string joined(const std::vector<std::string>& rows) {
    std::string text;
    for (const std::string& row : rows) {
        text += row + "\n";
    }
    return text;
}

/// Palabras separadas por espacios (para comparar sin importar el ajuste).
std::vector<std::string> words_of(const std::string& text) {
    std::vector<std::string> words;
    std::istringstream stream{text};
    std::string word;
    while (stream >> word) {
        words.push_back(word);
    }
    return words;
}

/// Posición (x, y) de la primera celda donde empieza needle en una fila.
struct Position {
    int x = -1;
    int y = -1;
};

Position find(const ftxui::Screen& screen, std::string_view needle) {
    for (int y = 0; y < screen.dimy(); ++y) {
        for (int x = 0; x < screen.dimx(); ++x) {
            std::string text;
            for (int k = x; k < screen.dimx() && text.size() < needle.size(); ++k) {
                text += screen.CellAt(k, y).character;
            }
            if (text.rfind(needle, 0) == 0) {
                return Position{x, y};
            }
        }
    }
    return Position{};
}

void check_fits(const std::vector<std::string>& rows, int width) {
    for (const std::string& row : rows) {
        INFO("fila: \"" << row << "\"");
        CHECK(ftxui::string_width(row) <= width);
    }
}

} // namespace

TEST_CASE("vista: párrafo ajustado con estilos", "[vista]") {
    const int width = GENERATE(20, 40, 80);
    CAPTURE(width);
    const std::string source =
        "Texto con **negritas** y *cursivas* que se ajusta al ancho disponible sin perder "
        "ninguna palabra ~~tachada~~ ni ==resaltada==.";
    const ftxui::Screen screen = draw(source, width);
    const auto rows = rows_of(screen);
    check_fits(rows, width);
    CHECK(words_of(joined(rows)) ==
          words_of("Texto con negritas y cursivas que se ajusta al ancho disponible sin perder "
                   "ninguna palabra tachada ni resaltada."));
    if (width == 20) {
        CHECK(rows.size() > 3);
    }
    const Position bold = find(screen, "negritas");
    REQUIRE(bold.x >= 0);
    CHECK(screen.CellAt(bold.x, bold.y).bold);
    CHECK_FALSE(screen.CellAt(0, 0).bold);
    const Position italic = find(screen, "cursivas");
    REQUIRE(italic.x >= 0);
    CHECK(screen.CellAt(italic.x, italic.y).italic);
    const Position strike = find(screen, "tachada");
    REQUIRE(strike.x >= 0);
    CHECK(screen.CellAt(strike.x, strike.y).strikethrough);
    const Position mark = find(screen, "resaltada");
    REQUIRE(mark.x >= 0);
    CHECK(screen.CellAt(mark.x, mark.y).background_color == ftxui::Color(ftxui::Color::Yellow));
}

TEST_CASE("vista: código en línea no se parte si cabe", "[vista]") {
    const ftxui::Screen screen = draw("aaaa bbbb cccc `ls -la /tmp` fin", 20);
    const auto rows = rows_of(screen);
    REQUIRE(rows.size() == 2);
    CHECK(rows[0] == "aaaa bbbb cccc");
    CHECK(rows[1] == "ls -la /tmp fin");
    const Position code = find(screen, "ls -la");
    CHECK(screen.CellAt(code.x, code.y).foreground_color == ftxui::Color(ftxui::Color::Cyan));
}

TEST_CASE("vista: enlace, imagen y nota al pie", "[vista]") {
    const ftxui::Screen screen =
        draw("Ve [el sitio](https://ej.com/a) o https://ej.org y ![logo](https://ej.com/l.png)"
             " y una nota[^n].\n\n[^n]: Texto de la nota.",
             80);
    const auto rows = rows_of(screen);
    CHECK(rows[0] == "Ve el sitio (https://ej.com/a) o https://ej.org y [imagen: logo]");
    CHECK(rows[1] == "(https://ej.com/l.png) y una nota[1].");
    const Position link = find(screen, "el sitio");
    const ftxui::Cell& cell = screen.CellAt(link.x, link.y);
    CHECK(cell.underlined);
    REQUIRE(cell.hyperlink != 0);
    CHECK(screen.Hyperlink(cell.hyperlink) == "https://ej.com/a");
    const Position url = find(screen, "(https://ej.com/a)");
    CHECK(screen.CellAt(url.x, url.y).dim);
    // La nota va al final, después de una línea.
    REQUIRE(rows.size() >= 3);
    CHECK(rows.back() == "[1] Texto de la nota.");
    CHECK(rows[rows.size() - 2].find("─") != std::string::npos);
}

TEST_CASE("vista: URL de hyperlink sin bytes de control", "[vista]") {
    CHECK(md::hyperlink_target("https://ej.com/a b\x1b\x07ñ") ==
          "https://ej.com/a%20b%1B%07%C3%B1");
}

TEST_CASE("vista: encabezados", "[vista]") {
    const ftxui::Screen screen = draw("# Uno\n\n## Dos\n\n### Tres", 40);
    const auto rows = rows_of(screen);
    REQUIRE(rows.size() == 5);
    CHECK(rows[0] == "Uno");
    CHECK(rows[1].empty()); // Línea en blanco entre bloques.
    CHECK(rows[2] == "Dos");
    CHECK(rows[4] == "Tres");
    CHECK(screen.CellAt(0, 0).bold);
    CHECK(screen.CellAt(0, 0).underlined);
    CHECK(screen.CellAt(0, 2).bold);
    CHECK_FALSE(screen.CellAt(0, 2).underlined);
    CHECK_FALSE(screen.CellAt(0, 2).dim);
    CHECK(screen.CellAt(0, 4).bold);
    CHECK(screen.CellAt(0, 4).dim);
}

TEST_CASE("vista: lista anidada con sangría colgante", "[vista]") {
    const int width = GENERATE(20, 40, 80);
    CAPTURE(width);
    const ftxui::Screen screen =
        draw("- primer elemento con un texto bastante largo para ajustar\n"
             "  - anidado con texto también largo que ocupa varias líneas\n"
             "    - tercer nivel\n"
             "- segundo",
             width);
    const auto rows = rows_of(screen);
    check_fits(rows, width);
    REQUIRE(!rows.empty());
    CHECK(rows[0].rfind("• primer", 0) == 0);
    // Cada línea de continuación respeta la sangría de su elemento.
    int indent = 0;
    for (const std::string& row : rows) {
        INFO("fila: \"" << row << "\"");
        if (row.rfind("• ", 0) == 0) {
            indent = 2;
        } else if (row.rfind("  ◦ ", 0) == 0) {
            indent = 4;
        } else if (row.rfind("    ▪ ", 0) == 0) {
            indent = 6;
        } else {
            CHECK(row.size() > static_cast<std::size_t>(indent));
            CHECK(row.find_first_not_of(' ') == static_cast<std::size_t>(indent));
        }
    }
    CHECK(rows.back() == "• segundo");
    if (width == 20) {
        CHECK(rows.size() > 6);
    }
}

TEST_CASE("vista: listas numeradas y tareas", "[vista]") {
    const auto rows = rows_of(draw("9. nueve\n10. diez\n\n- [ ] pendiente\n- [x] hecha", 40));
    REQUIRE(rows.size() == 5);
    CHECK(rows[0] == " 9. nueve");
    CHECK(rows[1] == "10. diez");
    CHECK(rows[3] == "☐ pendiente");
    CHECK(rows[4] == "☑ hecha");
}

TEST_CASE("vista: cita anidada", "[vista]") {
    const int width = GENERATE(20, 40, 80);
    CAPTURE(width);
    const ftxui::Screen screen = draw("> afuera\n>\n> > adentro", width);
    const auto rows = rows_of(screen);
    REQUIRE(rows.size() == 3);
    CHECK(rows[0] == "│ afuera");
    CHECK(rows[1] == "│");
    CHECK(rows[2] == "│ │ adentro");
    CHECK(screen.CellAt(0, 0).dim);
    CHECK(screen.CellAt(2, 2).dim);
    CHECK_FALSE(screen.CellAt(4, 2).dim);
}

TEST_CASE("vista: alerta con etiqueta", "[vista]") {
    const ftxui::Screen screen = draw("> [!WARNING]\n> Cuidado con esto.", 40);
    const auto rows = rows_of(screen);
    REQUIRE(rows.size() == 2);
    CHECK(rows[0] == "│ Advertencia");
    CHECK(rows[1] == "│ Cuidado con esto.");
    CHECK(screen.CellAt(2, 0).bold);
}

TEST_CASE("vista: bloque de código con línea larga partida", "[vista]") {
    const int width = GENERATE(20, 40, 80);
    CAPTURE(width);
    const std::string line = "    return 0; // comentario largo que no cabe en el ancho";
    const ftxui::Screen screen = draw("```cpp\nint main() {\n" + line + "\n}\n```", width);
    const auto rows = rows_of(screen);
    check_fits(rows, width);
    REQUIRE(rows.size() >= 5);
    CHECK(rows.front().find("cpp") != std::string::npos);
    // El contenido, sin el marco, es el código original partido por caracteres.
    std::vector<std::string> inner;
    for (std::size_t i = 1; i + 1 < rows.size(); ++i) {
        const std::string& row = rows[i];
        REQUIRE(row.rfind("│ ", 0) == 0);
        std::string content = row.substr(std::string{"│ "}.size());
        const std::size_t end = content.rfind(" │");
        REQUIRE(end != std::string::npos);
        content.erase(end);
        inner.push_back(content);
    }
    CHECK(inner.front().rfind("int main() {", 0) == 0);
    CHECK(inner[1].rfind("    return", 0) == 0); // Sangría intacta.
    std::string rebuilt;
    for (std::size_t i = 1; i + 1 < inner.size(); ++i) {
        std::string part = inner[i];
        if (i + 2 == inner.size()) {
            part.erase(part.find_last_not_of(' ') + 1); // Relleno del marco.
        }
        rebuilt += part;
    }
    CHECK(rebuilt == line);
    if (width < 60) {
        CHECK(inner.size() > 3);
    }
}

TEST_CASE("vista: tabla que cabe, con alineación", "[vista]") {
    const ftxui::Screen screen =
        draw("| Nombre | Edad | Ciudad |\n|:--|--:|:-:|\n| Ana | 30 | León |\n| Bo | 5 | X |", 80);
    const auto rows = rows_of(screen);
    REQUIRE(rows.size() == 6);
    CHECK(rows[0] == "┌────────┬──────┬────────┐");
    CHECK(rows[1] == "│ Nombre │ Edad │ Ciudad │");
    CHECK(rows[2] == "├────────┼──────┼────────┤");
    CHECK(rows[3] == "│ Ana    │   30 │  León  │");
    CHECK(rows[4] == "│ Bo     │    5 │   X    │");
    CHECK(rows[5] == "└────────┴──────┴────────┘");
    const Position header = find(screen, "Nombre");
    CHECK(screen.CellAt(header.x, header.y).bold);
    const Position body = find(screen, "Ana");
    CHECK_FALSE(screen.CellAt(body.x, body.y).bold);
}

TEST_CASE("vista: tabla que ajusta el texto en las celdas", "[vista]") {
    const std::string source =
        "| Uno | Dos | Tres |\n|---|---|---|\n"
        "| texto corto | un texto bastante más largo que el ancho | final |\n";
    const ftxui::Screen screen = draw(source, 40);
    const auto rows = rows_of(screen);
    check_fits(rows, 40);
    REQUIRE(rows.size() > 5);
    CHECK(rows[0].rfind("┌", 0) == 0);
    const int table_width = ftxui::string_width(rows[0]);
    std::string text;
    for (const std::string& row : rows) {
        INFO("fila: \"" << row << "\"");
        CHECK(ftxui::string_width(row) == table_width); // Bordes alineados.
        text += row + " ";
    }
    for (const std::string& word : words_of("Uno Dos Tres texto corto un texto bastante más largo "
                                            "que el ancho final")) {
        CHECK(text.find(word) != std::string::npos);
    }
}

TEST_CASE("vista: tabla demasiado ancha como tarjetas", "[vista]") {
    const std::string source = "| Nombre | Edad | Ciudad | País |\n|---|---|---|---|\n"
                               "| Ana | 30 | León | México |\n| Bo | 5 | Lima | Perú |\n";
    const ftxui::Screen screen = draw(source, 20);
    const auto rows = rows_of(screen);
    check_fits(rows, 20);
    CHECK(joined(rows).find("┌") == std::string::npos);
    REQUIRE(rows.size() == 9);
    CHECK(rows[0] == "Nombre: Ana");
    CHECK(rows[1] == "Edad: 30");
    CHECK(rows[2] == "Ciudad: León");
    CHECK(rows[3] == "País: México");
    CHECK(rows[4].find("─") != std::string::npos);
    CHECK(rows[5] == "Nombre: Bo");
    CHECK(rows[8] == "País: Perú");
    CHECK(screen.CellAt(0, 0).bold);
    CHECK_FALSE(screen.CellAt(8, 0).bold);
}

TEST_CASE("vista: emoji y CJK en párrafos y tablas", "[vista]") {
    const int width = GENERATE(20, 40, 80);
    CAPTURE(width);
    const std::string cjk = "汉字汉字汉字汉字汉字汉字汉字汉字汉字汉字";
    const ftxui::Screen paragraph = draw(cjk + " y 😀 emoji", width);
    const auto rows = rows_of(paragraph);
    check_fits(rows, width);
    std::string text;
    for (const std::string& row : rows) {
        text += row;
    }
    CHECK(text.find("😀") != std::string::npos);
    std::string only_cjk;
    for (const std::string& row : rows) {
        only_cjk += row.substr(0, row.find(' '));
    }
    CHECK(only_cjk.rfind(cjk, 0) == 0);

    const ftxui::Screen table = draw("| 名前 | 😀 |\n|---|---|\n| 東京 | ok |", width);
    const auto table_rows = rows_of(table);
    check_fits(table_rows, width);
    REQUIRE(!table_rows.empty());
    const int table_width = ftxui::string_width(table_rows[0]);
    for (const std::string& row : table_rows) {
        INFO("fila: \"" << row << "\"");
        CHECK(ftxui::string_width(row) == table_width);
    }
    CHECK(joined(table_rows).find("東京") != std::string::npos);
}

TEST_CASE("vista: las secuencias ESC del texto no llegan a la salida", "[vista]") {
    const int width = GENERATE(20, 40, 80);
    CAPTURE(width);
    const ftxui::Screen screen =
        draw("rojo \x1b[31m texto \x1b]8;;http://x\x07 y `\x1b[2J`\n\n```\n\x1b[1mcodigo\n```", width);
    const std::string text = joined(rows_of(screen));
    CHECK(text.find('\x1b') == std::string::npos);
    CHECK(text.find('\x07') == std::string::npos);
    CHECK(text.find("\xEF\xBF\xBD[31m") != std::string::npos);
    const std::string output = screen.ToString();
    CHECK(output.find("\x1b[31m") == std::string::npos);
    CHECK(output.find("\x1b[2J") == std::string::npos);
    CHECK(output.find("\x1b]8;;http://x") == std::string::npos);
}

TEST_CASE("vista: texto vacío y ancho mínimo", "[vista]") {
    CHECK(rows_of(draw("", 20)) == std::vector<std::string>{""});
    const auto rows = rows_of(draw("palabra **larga** y `codigo`\n\n> cita\n\n| a | b |\n|---|---|\n"
                                   "| 1 | 2 |",
                                   3));
    std::string text;
    for (const std::string& row : rows) {
        text += row;
    }
    for (const std::string_view piece : {"pal", "abr", "lar", "cod", "1", "2"}) {
        CHECK(text.find(piece) != std::string::npos);
    }
}
