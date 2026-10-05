#include "markdown.h"
#include "markdown_view.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/string.hpp>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace md = chatbot::cli::md;

/// Dibuja el markdown con el ancho dado y el alto que necesite.
ftxui::Screen draw(std::string_view markdown, int width) {
    ftxui::Element element = md::render(md::parse(markdown), width, chatbot::cli::terminal_palette());
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
    const ftxui::Screen screen = draw("# Uno\n\n## Dos\n\n### Tres\n\n###### Seis", 40);
    const auto rows = rows_of(screen);
    REQUIRE(rows.size() == 7);
    CHECK(rows[0] == "Uno");
    CHECK(rows[1].empty()); // Línea en blanco entre bloques.
    CHECK(rows[2] == "Dos");
    CHECK(rows[4] == "▍ Tres");
    CHECK(screen.CellAt(0, 0).bold);
    CHECK(screen.CellAt(0, 0).underlined);
    CHECK(screen.CellAt(0, 2).bold);
    CHECK_FALSE(screen.CellAt(0, 2).underlined);
    CHECK_FALSE(screen.CellAt(0, 2).dim);
    CHECK(screen.CellAt(0, 2).foreground_color == ftxui::Color(ftxui::Color::Default));
    // H3-H6: negritas en cian, sin dim (se leen sobre fondos translúcidos).
    // El texto del título empieza después de la marca "▍ ".
    CHECK(rows[6] == "▍ Seis");
    const int text_x = ftxui::string_width("▍ ");
    for (const int y : {4, 6}) {
        CAPTURE(y);
        CHECK(screen.CellAt(text_x, y).bold);
        CHECK_FALSE(screen.CellAt(text_x, y).dim);
        CHECK_FALSE(screen.CellAt(text_x, y).underlined);
        CHECK(screen.CellAt(text_x, y).foreground_color == ftxui::Color(ftxui::Color::Cyan));
    }
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

namespace {

std::string read_data(const std::string& name) {
    std::ifstream file{std::string{CHATBOT_TEST_DATA_DIR} + "/" + name, std::ios::binary};
    REQUIRE(file);
    std::ostringstream content;
    content << file.rdbuf();
    return content.str();
}

/// Texto plano de una celda (como se dibuja, sin estilos).
std::string cell_text(const md::Block& cell) {
    std::string text;
    for (const md::Run& run : cell.runs) {
        text += run.text;
    }
    return text;
}

/// Caracteres visibles, ordenados, sin espacios ni dibujo de bordes.
std::vector<std::string> glyphs_without_borders(const std::string& text) {
    static const std::vector<std::string> kIgnored{" ", "\n", "│", "┌", "┐", "└", "┘",
                                                   "├", "┤", "┬", "┴", "┼", "─"};
    std::vector<std::string> glyphs;
    for (const std::string& glyph : ftxui::Utf8ToGlyphs(text)) {
        if (!glyph.empty() &&
            std::find(kIgnored.begin(), kIgnored.end(), glyph) == kIgnored.end()) {
            glyphs.push_back(glyph);
        }
    }
    std::sort(glyphs.begin(), glyphs.end());
    return glyphs;
}

/// Altos de las filas de una tabla dibujada (líneas entre separadores).
std::vector<int> row_heights(const std::vector<std::string>& rows) {
    std::vector<int> heights;
    int current = 0;
    for (const std::string& row : rows) {
        if (row.rfind("│", 0) == 0) {
            ++current;
        } else if (current > 0) {
            heights.push_back(current);
            current = 0;
        }
    }
    return heights;
}

} // namespace

TEST_CASE("vista: el reparto del ancho minimiza el alto de la tabla", "[vista][tabla]") {
    const auto rows = rows_of(draw(read_data("tabla_reparto.md"), 117));
    check_fits(rows, 117);
    REQUIRE(rows.front().rfind("┌", 0) == 0);
    const std::vector<int> heights = row_heights(rows);
    REQUIRE(heights.size() == 4); // Encabezado y tres filas.
    for (const int height : heights) {
        INFO(joined(rows));
        CHECK(height <= 3);
    }
    CHECK(heights.front() == 1); // El encabezado no parte palabras.
}

TEST_CASE("vista: una tabla que cabe conserva sus anchos naturales", "[vista][tabla]") {
    const md::Document document =
        md::parse("| Nombre | Edad | Ciudad |\n|:--|--:|:-:|\n| Ana | 30 | León |\n| Bo | 5 | X |");
    REQUIRE(document.blocks.size() == 1);
    // Mide 26 con bordes (6 + 4 + 6 + 3 * 3 + 1): desde ahí va completa.
    for (const int width : {26, 27, 28, 40, 80, 200}) {
        CAPTURE(width);
        CHECK(md::table_column_widths(document.blocks.front(), width) == std::vector<int>{6, 4, 6});
    }
    // A 25 quedan 15 columnas y los pisos, min(natural, 6), suman 16: tarjetas.
    CHECK(md::table_column_widths(document.blocks.front(), 25).empty());
}

TEST_CASE("vista: la tabla en fichas no cambia", "[vista][tabla]") {
    const auto rows = rows_of(draw(read_data("tabla_reparto.md"), 40));
    CHECK(joined(rows) == read_data("tabla_reparto_fichas_40.txt"));
}

TEST_CASE("vista: ninguna tabla pierde texto en ningún ancho", "[vista][tabla]") {
    const std::string source = read_data("tabla_reparto.md");
    const md::Document document = md::parse(source);
    REQUIRE(document.blocks.size() == 1);
    const md::Block& table = document.blocks.front();
    REQUIRE(table.kind == md::Block::Kind::Table);
    REQUIRE(table.header_rows == 1);
    // Texto que debe aparecer como tabla y como fichas ("Encabezado: valor").
    std::string as_table;
    std::string as_cards;
    for (std::size_t r = 0; r < table.rows.size(); ++r) {
        for (std::size_t c = 0; c < table.rows[r].size(); ++c) {
            as_table += cell_text(table.rows[r][c]);
            if (r >= table.header_rows) {
                as_cards += cell_text(table.rows[0][c]) + ":" + cell_text(table.rows[r][c]);
            }
        }
    }
    const std::vector<std::string> expected_table = glyphs_without_borders(as_table);
    const std::vector<std::string> expected_cards = glyphs_without_borders(as_cards);
    for (int width = 30; width <= 140; ++width) {
        CAPTURE(width);
        const std::string drawn = joined(rows_of(draw(source, width)));
        const bool cards = drawn.find("┌") == std::string::npos;
        CHECK(glyphs_without_borders(drawn) == (cards ? expected_cards : expected_table));
    }
}

TEST_CASE("vista: tiempo del reparto de ancho de tablas", "[vista][tabla][desempeno]") {
    // Tabla grande: 10 columnas y 50 filas con textos de largos distintos.
    std::string big = "|";
    for (int c = 0; c < 10; ++c) {
        big += " Columna " + std::to_string(c) + " |";
    }
    big += "\n|";
    for (int c = 0; c < 10; ++c) {
        big += "---|";
    }
    big += "\n";
    const std::string words = "texto de relleno con palabras de largo variable para la celda ";
    for (int r = 0; r < 50; ++r) {
        big += "|";
        for (int c = 0; c < 10; ++c) {
            std::string cell;
            const int repeat = 1 + (r * 7 + c * 3) % 6;
            for (int k = 0; k < repeat; ++k) {
                cell += words;
            }
            big += " " + cell + "|";
        }
        big += "\n";
    }
    const md::Document large = md::parse(big);
    const md::Document small = md::parse(read_data("tabla_reparto.md"));
    // El mejor de tres, para que una pausa de la máquina no cuente.
    const auto best_of_three = [](const auto& work) {
        double best = 1e9;
        for (int i = 0; i < 3; ++i) {
            const auto start = std::chrono::steady_clock::now();
            work();
            best = std::min(best, std::chrono::duration<double, std::milli>(
                                      std::chrono::steady_clock::now() - start)
                                      .count());
        }
        return best;
    };
    const auto reparto = [&](const md::Document& document, int width) {
        return best_of_three([&] {
            CHECK_FALSE(md::table_column_widths(document.blocks.front(), width).empty());
        });
    };
    const auto dibujo = [&](const md::Document& document, int width) {
        return best_of_three([&] { (void)md::render(document, width, chatbot::cli::terminal_palette()); });
    };
    const double small_ms = reparto(small, 117);
    const double large_120 = reparto(large, 120);
    const double large_200 = reparto(large, 200);
    WARN("reparto: tabla de prueba (117 col.) " << small_ms << " ms; tabla de 10x50 "
                                                << large_120 << " ms a 120 col., " << large_200
                                                << " ms a 200 col.");
    WARN("dibujo completo: tabla de prueba (117 col.) "
         << dibujo(small, 117) << " ms; tabla de 10x50 " << dibujo(large, 120)
         << " ms a 120 col., " << dibujo(large, 200) << " ms a 200 col.");
#if defined(NDEBUG) && !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_THREAD__)
    CHECK(large_120 < 20.0);
    CHECK(large_200 < 20.0);
#endif
}

TEST_CASE("vista: una tabla angosta que cabe no pasa a fichas", "[vista][tabla]") {
    const std::string source = read_data("tabla_angosta.md");
    const md::Document document = md::parse(source);
    REQUIRE(document.blocks.size() == 1);
    const md::Block& table = document.blocks.front();
    const std::vector<std::string> natural{
        "┌────┬────┬──────────────┐",
        "│ N° │ Ok │ Producto     │",
        "├────┼────┼──────────────┤",
        "│  1 │ sí │ Café molido  │",
        "│  2 │ no │ Pan de trigo │",
        "│ 10 │ sí │ Leche        │",
        "└────┴────┴──────────────┘",
    };
    // Anchos naturales 2, 2 y 12: dibujada completa mide 26 columnas.
    for (const int width : {26, 30}) {
        CAPTURE(width);
        CHECK(md::table_column_widths(table, width) == std::vector<int>{2, 2, 12});
        CHECK(rows_of(draw(source, width)) == natural);
    }

    // A 20: quedan 20 - (3 * 3 + 1) = 10 columnas y los pisos suman
    // min(2, 6) + min(2, 6) + min(12, 6) = 10. Cabe: tabla, con el texto de
    // la última columna ajustado.
    CHECK(md::table_column_widths(table, 20) == std::vector<int>{2, 2, 6});
    const auto rows = rows_of(draw(source, 20));
    check_fits(rows, 20);
    REQUIRE(rows.front().rfind("┌", 0) == 0);
    std::string as_table;
    for (const auto& row : table.rows) {
        for (const md::Block& cell : row) {
            as_table += cell_text(cell);
        }
    }
    CHECK(glyphs_without_borders(joined(rows)) == glyphs_without_borders(as_table));
    CHECK(joined(rows).find("│ 10 │ sí │ Leche  │") != std::string::npos);

    // A 19 quedan 9 < 10: ahora sí, fichas.
    CHECK(md::table_column_widths(table, 19).empty());
    CHECK(joined(rows_of(draw(source, 19))).find("┌") == std::string::npos);
}

TEST_CASE("vista: una columna vacía no queda de ancho 0", "[vista][tabla]") {
    const std::string source = "| A |  | C |\n|---|---|---|\n| uno |  | tres |\n| dos |  | |";
    const md::Document document = md::parse(source);
    REQUIRE(document.blocks.size() == 1);
    for (int width = 1; width <= 40; ++width) {
        CAPTURE(width);
        const std::vector<int> widths = md::table_column_widths(document.blocks.front(), width);
        for (const int w : widths) {
            CHECK(w >= 1);
        }
        const auto rows = rows_of(draw(source, width));
        check_fits(rows, width);
        if (!widths.empty()) {
            // Todas las filas de la tabla miden lo mismo: bordes alineados.
            REQUIRE(widths.size() == 3);
            const int table_width = ftxui::string_width(rows.front());
            for (const std::string& row : rows) {
                CHECK(ftxui::string_width(row) == table_width);
            }
            CHECK(rows.front().find("┬┬") == std::string::npos);
        }
    }
    // Anchos naturales 3, 1 y 4: a 18 (8 + 3 * 3 + 1) va completa.
    CHECK(md::table_column_widths(document.blocks.front(), 18) == std::vector<int>{3, 1, 4});
}

namespace {

/// Columna del último carácter visible de la fila y, o -1 si no hay.
int last_visible(const ftxui::Screen& screen, int y) {
    int last = -1;
    for (int x = 0; x < screen.dimx(); ++x) {
        const std::string& character = screen.CellAt(x, y).character;
        if (!character.empty() && character != " ") {
            last = x;
        }
    }
    return last;
}

/// Ninguna celda después del último carácter de cada fila lleva subrayado,
/// tachado, fondo ni hipervínculo: los decoradores cubren solo el texto. Con
/// strict, tampoco negritas, dim ni color de letra (no se ven sobre espacios,
/// pero tampoco deben estirarse).
void check_no_style_past_text(const ftxui::Screen& screen, bool strict = false) {
    for (int y = 0; y < screen.dimy(); ++y) {
        for (int x = last_visible(screen, y) + 1; x < screen.dimx(); ++x) {
            CAPTURE(x, y);
            const ftxui::Cell& cell = screen.CellAt(x, y);
            CHECK_FALSE(cell.underlined);
            CHECK_FALSE(cell.strikethrough);
            CHECK(cell.background_color == ftxui::Color(ftxui::Color::Default));
            CHECK(cell.hyperlink == 0);
            if (strict) {
                CHECK_FALSE(cell.bold);
                CHECK_FALSE(cell.dim);
                CHECK(cell.foreground_color == ftxui::Color(ftxui::Color::Default));
            }
        }
    }
}

} // namespace

TEST_CASE("vista: el estilo de una línea no se estira hasta el borde", "[vista]") {
    struct Case {
        const char* source;
        const char* text; ///< Texto que sí lleva el estilo.
    };
    const Case cases[] = {
        {"# Título", "Título"},
        {"<https://es.wikipedia.org>", "https://es.wikipedia.org"},
        {"==resaltado==", "resaltado"},
        {"~~tachado~~", "tachado"},
    };
    for (const Case& c : cases) {
        CAPTURE(c.source);
        const ftxui::Screen screen = draw(c.source, 40);
        REQUIRE(rows_of(screen).front() == c.text);
        // El texto sí lleva su estilo...
        const ftxui::Cell& first = screen.CellAt(0, 0);
        CHECK((first.underlined || first.strikethrough || first.hyperlink != 0 ||
               first.background_color != ftxui::Color(ftxui::Color::Default)));
        // ...y el relleno a la derecha, no.
        check_no_style_past_text(screen, true);
    }
}

TEST_CASE("vista: el estilo no se estira en tablas, notas, citas, listas ni código",
          "[vista]") {
    const char* sources[] = {
        "| A | B |\n|---|---|\n| [x](https://ej.com) | ~~y~~ |",
        "Texto[^1].\n\n[^1]: [nota](https://ej.com)",
        "> [cita](https://ej.com)",
        "- ==uno==\n- ~~dos~~",
        "```cpp\nint x;\n```",
        "> [!NOTE]\n> ~~alerta~~",
    };
    for (const char* source : sources) {
        CAPTURE(source);
        check_no_style_past_text(draw(source, 40), true);
    }
}

TEST_CASE("vista: H3-H6 llevan la marca y H1-H2 no", "[vista]") {
    const std::string mark = "▍ ";
    for (int level = 1; level <= 6; ++level) {
        CAPTURE(level);
        const std::string source = std::string(static_cast<std::size_t>(level), '#') + " Título";
        const auto rows = rows_of(draw(source, 40));
        REQUIRE(rows.size() == 1);
        if (level >= 3) {
            CHECK(rows.front() == mark + "Título");
        } else {
            CHECK(rows.front() == "Título");
        }
    }
    // Solo es dibujo: el árbol de markdown no lleva la marca.
    const md::Document document = md::parse("### Título");
    REQUIRE(document.blocks.size() == 1);
    REQUIRE(document.blocks.front().runs.size() == 1);
    CHECK(document.blocks.front().runs.front().text == "Título");
}

TEST_CASE("vista: la marca lleva el estilo del título y no se estira", "[vista]") {
    const int mark_width = ftxui::string_width("▍ ");
    for (const char* source : {"### Subtítulo", "#### Subtítulo", "###### Subtítulo"}) {
        CAPTURE(source);
        const ftxui::Screen screen = draw(source, 40);
        for (int x = 0; x < mark_width; ++x) {
            CAPTURE(x);
            CHECK(screen.CellAt(x, 0).bold);
            CHECK(screen.CellAt(x, 0).foreground_color == ftxui::Color(ftxui::Color::Cyan));
            CHECK_FALSE(screen.CellAt(x, 0).dim);
            CHECK_FALSE(screen.CellAt(x, 0).underlined);
        }
        CHECK(screen.CellAt(0, 0).character == "▍");
        // Después del último carácter del título no hay ningún estilo.
        check_no_style_past_text(screen, true);
    }
}

TEST_CASE("vista: un H3 largo se ajusta con sangría colgante de 2", "[vista]") {
    const std::string title = "Un subtítulo bastante largo que ocupa varias líneas";
    const ftxui::Screen screen = draw("### " + title, 30);
    const auto rows = rows_of(screen);
    check_fits(rows, 30);
    REQUIRE(rows.size() >= 2);
    const int mark_width = ftxui::string_width("▍ ");
    REQUIRE(mark_width == 2);
    CHECK(rows.front().rfind("▍ ", 0) == 0);
    for (std::size_t i = 1; i < rows.size(); ++i) {
        CAPTURE(rows[i]);
        // Alineada con el texto del título, no con la marca.
        CHECK(rows[i].find_first_not_of(' ') == static_cast<std::size_t>(mark_width));
        CHECK(rows[i].find("▍") == std::string::npos);
    }
    // Ninguna palabra se pierde y la sangría no lleva estilo.
    std::string text = joined(rows);
    text.erase(0, std::string{"▍"}.size());
    CHECK(words_of(text) == words_of(title));
    for (int y = 1; y < screen.dimy(); ++y) {
        for (int x = 0; x < mark_width; ++x) {
            CAPTURE(x, y);
            CHECK_FALSE(screen.CellAt(x, y).bold);
            CHECK(screen.CellAt(x, y).foreground_color == ftxui::Color(ftxui::Color::Default));
        }
    }
    check_no_style_past_text(screen, true);
}
