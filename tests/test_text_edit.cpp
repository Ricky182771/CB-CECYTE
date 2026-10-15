// Modelo de los campos de varias líneas (text_edit.h): distribución en filas,
// cursor con afinidad, movimiento, edición, vista y rendimiento.

#include "text_edit.h"

#include "performance.hpp"

#include "chatbot/utf8.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/string.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace {

using chatbot::cli::Affinity;
using chatbot::cli::ColumnRule;
using chatbot::cli::RowBreak;
using chatbot::cli::RowColumn;
using chatbot::cli::TextCursor;
using chatbot::cli::TextEdit;
using chatbot::cli::TextRow;

/// El texto de cada fila.
std::vector<std::string> row_texts(std::string_view text, int width) {
    std::vector<std::string> out;
    for (const TextRow& row : chatbot::cli::layout(text, width)) {
        out.emplace_back(text.substr(row.start, row.end - row.start));
    }
    return out;
}

/// Todos los límites de glifo, de 0 a text.size().
std::vector<std::size_t> boundaries(std::string_view text) {
    std::vector<std::size_t> out{0};
    for (std::size_t at = 0; at < text.size();) {
        at = chatbot::cli::next_glyph(text, at);
        out.push_back(at);
    }
    return out;
}

bool is_boundary(std::string_view text, std::size_t offset) {
    const std::vector<std::size_t> all = boundaries(text);
    return std::find(all.begin(), all.end(), offset) != all.end();
}

/// Piezas para los textos aleatorios: letras con acento, espacios, saltos,
/// emoji (con modificador), ancho 2, combinantes y algún "\t".
constexpr std::string_view kPieces[] = {
    "a",    "b",  "ñ",  "é",          " ", " ", "  ", "\n", "中", "文", "👍", "👍🏽",
    "e\xCC\x81" /* e + U+0301 */, "\xCC\x81" /* U+0301 suelto */, "palabra", "\t", "x",
};

std::string random_text(std::mt19937& random, int pieces) {
    std::uniform_int_distribution<std::size_t> pick(0, std::size(kPieces) - 1);
    std::string text;
    for (int i = 0; i < pieces; ++i) {
        text += kPieces[pick(random)];
    }
    return text;
}

/// true si en offset hay un espacio " " (con o sin combinantes).
bool space_at(std::string_view text, std::size_t offset) {
    if (offset >= text.size()) {
        return false;
    }
    const chatbot::cli::TextGlyph glyph = chatbot::cli::glyph_at(text, offset);
    return glyph.kind == chatbot::cli::GlyphKind::Normal && glyph.base_end == offset + 1 &&
           text[offset] == ' ';
}

/// Revisa lo que siempre debe cumplir layout(text, width).
void check_layout(std::string_view text, int width) {
    const std::vector<TextRow> rows = chatbot::cli::layout(text, width);
    REQUIRE_FALSE(rows.empty());
    // La concatenación de las filas (más los "\n") es el texto.
    std::string joined;
    std::size_t expected_start = 0;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const TextRow& row = rows[i];
        INFO("fila " << i << " ancho " << width);
        REQUIRE(row.start == expected_start);
        REQUIRE(row.end >= row.start);
        joined += text.substr(row.start, row.end - row.start);
        // El ancho es la suma de los glifos. Ninguna fila pasa del ancho
        // completo, y sin espacio colgado tampoco del útil (salvo una fila de
        // un solo glifo, más ancho que el campo).
        int cells = 0;
        int glyphs = 0;
        std::size_t last_glyph = row.start;
        for (std::size_t at = row.start; at < row.end;) {
            const chatbot::cli::TextGlyph glyph = chatbot::cli::glyph_at(text, at);
            REQUIRE(glyph.kind != chatbot::cli::GlyphKind::Newline);
            cells += glyph.width;
            ++glyphs;
            last_glyph = at;
            at = glyph.end;
            REQUIRE(at <= row.end);
        }
        CHECK(cells == row.width);
        CHECK((row.width <= width || glyphs == 1));
        if (row.hanging) {
            // Lo que cupo llena justo el ancho útil, y el espacio va en la
            // columna reservada.
            CHECK(row.ends == RowBreak::Soft);
            CHECK(glyphs >= 2);
            CHECK(row.width == width);
            CHECK(space_at(text, last_glyph));
        } else {
            CHECK((row.width <= std::max(0, width - 1) || glyphs == 1));
        }
        // Una fila solo empieza con espacio si la línea empieza con espacio
        // o si en el corte había dos o más seguidos (cuando cada glifo cabe en
        // el ancho útil: con menos, un glifo por fila).
        if (width > chatbot::cli::kTabWidth && i > 0 && rows[i - 1].ends == RowBreak::Soft &&
            space_at(text, row.start)) {
            CHECK(space_at(text, chatbot::cli::previous_glyph(text, row.start)));
        }
        if (row.ends == RowBreak::Soft) {
            CHECK(row.end > row.start); // Una fila cortada nunca está vacía.
            expected_start = row.end;
        } else if (row.ends == RowBreak::Newline) {
            REQUIRE(row.end < text.size());
            CHECK(text[row.end] == '\n');
            joined += '\n';
            expected_start = row.end + 1;
        } else {
            CHECK(i + 1 == rows.size());
            CHECK(row.end == text.size());
        }
    }
    CHECK(rows.back().ends == RowBreak::End);
    CHECK(joined == text);
}

/// Cada límite de glifo va a (fila, columna) y de regreso al mismo lugar.
void check_round_trip(std::string_view text, int width) {
    const std::vector<TextRow> rows = chatbot::cli::layout(text, width);
    for (const std::size_t offset : boundaries(text)) {
        for (const Affinity affinity : {Affinity::Down, Affinity::Up}) {
            const TextCursor cursor{offset, affinity};
            const RowColumn at = chatbot::cli::to_row_column(text, rows, cursor);
            const TextCursor back = chatbot::cli::from_row_column(text, rows, at.row, at.column,
                                                                  ColumnRule::Floor);
            INFO("offset " << offset << " ancho " << width << " fila " << at.row << " columna "
                           << at.column);
            CHECK(back.offset == offset);
            CHECK(at.column <= rows[static_cast<std::size_t>(at.row)].width);
            // El cursor siempre cabe en el campo (si cada glifo cabe en el
            // ancho útil).
            if (width > chatbot::cli::kTabWidth) {
                CHECK(at.column <= width - 1);
            }
        }
    }
}

/// Un TextEdit con el texto y el cursor en offset.
TextEdit edit_at(std::string text, std::size_t offset) {
    TextEdit edit;
    edit.set_text(std::move(text), offset);
    return edit;
}

} // namespace

TEST_CASE("text_edit: debajo de U+0300 todo mide una celda", "[text_edit]") {
    // code_width se salta la tabla de FTXUI debajo de U+0300: aquí se revisa
    // que la tabla diga lo mismo (sin combinantes ni anchos dobles).
    for (std::uint32_t code = 0xA0; code < 0x300; ++code) {
        std::string bytes;
        bytes += static_cast<char>(0xC0 | (code >> 6));
        bytes += static_cast<char>(0x80 | (code & 0x3F));
        INFO("U+" << std::hex << code);
        CHECK(ftxui::string_width(bytes) == 1);
    }
    for (char c = ' '; c < 0x7F; ++c) {
        CHECK(ftxui::string_width(std::string(1, c)) == 1);
    }
}

TEST_CASE("text_edit: glifos", "[text_edit]") {
    using chatbot::cli::GlyphKind;
    SECTION("Un emoji con modificador y una letra con combinante son un glifo") {
        const std::string text = "👍🏽e\xCC\x81!";
        const chatbot::cli::TextGlyph thumb = chatbot::cli::glyph_at(text, 0);
        CHECK(thumb.end == 8);
        CHECK(thumb.width == 2);
        CHECK(thumb.kind == GlyphKind::Normal);
        const chatbot::cli::TextGlyph e = chatbot::cli::glyph_at(text, 8);
        CHECK(e.end == 11);
        CHECK(e.width == 1);
        CHECK(chatbot::cli::previous_glyph(text, 11) == 8);
        CHECK(chatbot::cli::previous_glyph(text, 8) == 0);
    }
    SECTION("Un combinante al principio de una línea es su propio glifo") {
        const std::string text = "\xCC\x81\xCC\x81x\n\xCC\x81";
        const chatbot::cli::TextGlyph first = chatbot::cli::glyph_at(text, 0);
        CHECK(first.kind == GlyphKind::Baseless);
        CHECK(first.width == 1);
        CHECK(first.end == 4); // Los dos combinantes juntos.
        CHECK(chatbot::cli::previous_glyph(text, 4) == 0);
        CHECK(chatbot::cli::glyph_at(text, 6).kind == GlyphKind::Baseless);
        CHECK(chatbot::cli::previous_glyph(text, text.size()) == 6);
        CHECK(chatbot::cli::glyph_display(text, first) == " " + text.substr(0, 4));
    }
    SECTION("\\t mide 4 celdas") {
        const chatbot::cli::TextGlyph tab = chatbot::cli::glyph_at("\tx", 0);
        CHECK(tab.kind == GlyphKind::Tab);
        CHECK(tab.width == chatbot::cli::kTabWidth);
        CHECK(tab.end == 1);
    }
    SECTION("UTF-8 inválido y controles: una celda, como U+FFFD") {
        const std::string text = "a\xC3z\xE2\x82\x1B\x7F\xC2\x85";
        std::vector<std::size_t> expected{0, 1, 2, 3, 4, 5, 6, 7, 9};
        CHECK(boundaries(text) == expected);
        for (std::size_t i = 1; i + 1 < expected.size(); ++i) {
            if (i == 2) {
                continue; // La "z".
            }
            const chatbot::cli::TextGlyph glyph = chatbot::cli::glyph_at(text, expected[i]);
            INFO("byte " << expected[i]);
            CHECK(glyph.kind == GlyphKind::Replacement);
            CHECK(glyph.width == 1);
            CHECK(chatbot::cli::glyph_display(text, glyph) ==
                  std::string{chatbot::utf8::kReplacement});
        }
        // Hacia atrás da los mismos límites que hacia adelante.
        for (std::size_t i = expected.size() - 1; i > 0; --i) {
            CHECK(chatbot::cli::previous_glyph(text, expected[i]) == expected[i - 1]);
        }
    }
    SECTION("glyph_boundary_at_or_before") {
        const std::string text = "añ中x";
        CHECK(chatbot::cli::glyph_boundary_at_or_before(text, 2) == 1);
        CHECK(chatbot::cli::glyph_boundary_at_or_before(text, 3) == 3);
        CHECK(chatbot::cli::glyph_boundary_at_or_before(text, 5) == 3);
        CHECK(chatbot::cli::glyph_boundary_at_or_before(text, 99) == text.size());
    }
}

TEST_CASE("text_edit: distribución", "[text_edit]") {
    using V = std::vector<std::string>;
    SECTION("Texto vacío: una fila") {
        const std::vector<TextRow> rows = chatbot::cli::layout("", 10);
        REQUIRE(rows.size() == 1);
        CHECK(rows[0] == TextRow{0, 0, 0, RowBreak::End});
    }
    SECTION("Ancho útil = ancho - 1") {
        CHECK(row_texts("abcd", 5) == V{"abcd"});
        CHECK(row_texts("abcde", 5) == V{"abcd", "e"});
    }
    SECTION("Se corta después del último espacio que cabe") {
        CHECK(row_texts("hola mundo feliz", 12) == V{"hola mundo ", "feliz"});
        CHECK(row_texts("uno dos tres", 9) == V{"uno dos ", "tres"});
        const std::vector<TextRow> rows = chatbot::cli::layout("uno dos tres", 9);
        CHECK(rows[0].ends == RowBreak::Soft);
        CHECK(rows[0].width == 8);
    }
    SECTION("Un espacio que ya no cabe se queda al final, en la columna reservada") {
        // "mundo" llena el ancho útil: no baja por el espacio que la sigue, y
        // ninguna fila empieza con espacio.
        CHECK(row_texts("hola mundo feliz", 11) == V{"hola mundo ", "feliz"});
        CHECK(row_texts("uno dos tres", 8) == V{"uno dos ", "tres"});
        const std::vector<TextRow> rows = chatbot::cli::layout("abcd efg", 5);
        REQUIRE(rows.size() == 2);
        CHECK(rows[0] == TextRow{0, 5, 5, RowBreak::Soft, true});
        CHECK(rows[1] == TextRow{5, 8, 3, RowBreak::End, false});
    }
    SECTION("Con dos espacios en el corte, solo el primero se queda; el otro baja") {
        CHECK(row_texts("abcd  efg", 5) == V{"abcd ", " efg"});
        const std::vector<TextRow> rows = chatbot::cli::layout("abcd  efg", 5);
        CHECK(rows[0].hanging);
        CHECK_FALSE(rows[1].hanging);
    }
    SECTION("Un espacio colgado al final de la línea deja una fila vacía para el cursor") {
        const std::vector<TextRow> rows = chatbot::cli::layout("abcd ", 5);
        REQUIRE(rows.size() == 2);
        CHECK(rows[0] == TextRow{0, 5, 5, RowBreak::Soft, true});
        CHECK(rows[1] == TextRow{5, 5, 0, RowBreak::End, false});
        CHECK(row_texts("abcd \nxy", 5) == V{"abcd ", "", "xy"});
        CHECK(chatbot::cli::layout("abcd \nxy", 5)[1].ends == RowBreak::Newline);
    }
    SECTION("Un \\t que no cabe no se queda colgado: baja") {
        CHECK(row_texts("abcd\tx", 5) == V{"abcd", "\t", "x"});
    }
    SECTION("Una palabra más larga que la fila se parte por glifos") {
        CHECK(row_texts("abcdefghij", 5) == V{"abcd", "efgh", "ij"});
        CHECK(row_texts("ab cdefghij", 5) == V{"ab ", "cdef", "ghij"});
    }
    SECTION("Varios espacios seguidos no se tiran ni se juntan") {
        CHECK(row_texts("a   b", 4) == V{"a   ", "b"});
        CHECK(row_texts("a          b", 5) == V{"a    ", "     ", " b"});
        CHECK(row_texts("     ", 3) == V{"   ", "  "});
    }
    SECTION("\\n termina la fila y al final deja una fila vacía") {
        CHECK(row_texts("ab\n", 10) == V{"ab", ""});
        CHECK(row_texts("\n\n", 10) == V{"", "", ""});
        const std::vector<TextRow> rows = chatbot::cli::layout("ab\ncd", 10);
        REQUIRE(rows.size() == 2);
        CHECK(rows[0] == TextRow{0, 2, 2, RowBreak::Newline});
        CHECK(rows[1] == TextRow{3, 5, 2, RowBreak::End});
    }
    SECTION("Un glifo de ancho 2 que no cabe en la última celda pasa completo") {
        CHECK(row_texts("abc中", 5) == V{"abc", "中"});
        CHECK(row_texts("ab中", 5) == V{"ab中"});
        CHECK(chatbot::cli::layout("abc中", 5)[0].width == 3);
    }
    SECTION("Un combinante nunca se separa de su base") {
        const std::string text = "abcde\xCC\x81" "f";
        CHECK(row_texts(text, 5) == V{"abcd", "e\xCC\x81" "f"});
        CHECK(row_texts(text, 6) == V{"abcde\xCC\x81", "f"});
    }
    SECTION("UTF-8 inválido: un glifo de una celda, sin ciclos") {
        CHECK(row_texts("ab\xFF\xFE" "cd", 3) == V{"ab", "\xFF\xFE", "cd"});
    }
    SECTION("\\t: un solo glifo de 4 celdas") {
        CHECK(row_texts("a\tb", 6) == V{"a\t", "b"});
        CHECK(chatbot::cli::layout("a\tb", 7)[0].width == 6);
        CHECK(row_texts("\t\t", 5) == V{"\t", "\t"});
    }
    SECTION("Anchos 0, 1 y 2: una fila por glifo, sin ciclos") {
        for (const int width : {0, 1}) {
            CHECK(row_texts("ab 中", width) == V{"a", "b", " ", "中"});
            CHECK(row_texts("", width) == V{""});
            CHECK(row_texts("a\nb", width) == V{"a", "b"});
        }
        CHECK(row_texts("ab 中", 2) == V{"a", "b ", "中"}); // El espacio, colgado.
    }
    SECTION("Una fila que llena exactamente el ancho útil") {
        const std::vector<TextRow> rows = chatbot::cli::layout("abcd", 5);
        REQUIRE(rows.size() == 1);
        CHECK(rows[0].width == 4);
        CHECK(rows[0].ends == RowBreak::End);
    }
}

TEST_CASE("text_edit: propiedades con textos aleatorios", "[text_edit]") {
    std::mt19937 random(20261014); // Semilla fija: la prueba siempre es la misma.
    std::uniform_int_distribution<int> widths(0, 30);
    std::uniform_int_distribution<int> lengths(0, 60);
    for (int round = 0; round < 300; ++round) {
        const std::string text = random_text(random, lengths(random));
        const int width = widths(random);
        INFO("ronda " << round);
        check_layout(text, width);
        check_round_trip(text, width);
    }
}

TEST_CASE("text_edit: la distribución incremental es igual a la completa", "[text_edit]") {
    std::mt19937 random(7);
    std::uniform_int_distribution<int> widths(0, 25);
    std::uniform_int_distribution<int> operations(0, 9);
    for (int round = 0; round < 120; ++round) {
        TextEdit edit(random_text(random, 40));
        const int width = widths(random);
        const int other = widths(random);
        for (int step = 0; step < 40; ++step) {
            (void)edit.rows(width); // En la caché antes de editar.
            (void)edit.rows(other);
            switch (operations(random)) {
            case 0:
            case 1:
            case 2:
                edit.insert(random_text(random, 1 + static_cast<int>(random() % 3)));
                break;
            case 3:
                (void)edit.erase_before();
                break;
            case 4:
                (void)edit.erase_after();
                break;
            case 5:
                (void)edit.move_left();
                break;
            case 6:
                (void)edit.move_up(width);
                break;
            case 7:
                (void)edit.move_down(width);
                break;
            case 8:
                edit.toggle_overwrite();
                edit.type("q");
                break;
            default:
                edit.click(static_cast<int>(random() % 10), static_cast<int>(random() % 30),
                           width);
                break;
            }
            INFO("ronda " << round << " paso " << step << " texto [" << edit.text() << "]");
            REQUIRE(*edit.rows(width) == chatbot::cli::layout(edit.text(), width));
            REQUIRE(*edit.rows(other) == chatbot::cli::layout(edit.text(), other));
            // El cursor siempre en un límite de glifo, dentro del texto, y visible.
            REQUIRE(edit.cursor().offset <= edit.text().size());
            REQUIRE(is_boundary(edit.text(), edit.cursor().offset));
            const int height = 3;
            const int top = edit.top_row(width, height);
            const int row = edit.cursor_position(width).row;
            REQUIRE(row >= top);
            REQUIRE(row < top + height);
        }
    }
}

TEST_CASE("text_edit: afinidad del cursor en un corte suave", "[text_edit]") {
    // "hola mundo " | "feliz" con ancho 12 (útil 11).
    const std::string text = "hola mundo feliz";
    constexpr int kWidth = 12;
    SECTION("Fin en una fila cortada se queda arriba, en la columna reservada") {
        TextEdit edit = edit_at(text, 0);
        REQUIRE(edit.move_end(kWidth));
        CHECK(edit.cursor() == TextCursor{11, Affinity::Up});
        CHECK(edit.cursor_position(kWidth) == RowColumn{0, 11});
        SECTION("→ la regresa abajo") {
            REQUIRE(edit.move_right());
            CHECK(edit.cursor() == TextCursor{12, Affinity::Down});
        }
        SECTION("Inicio la regresa abajo") {
            REQUIRE(edit.move_home(kWidth));
            CHECK(edit.cursor() == TextCursor{0, Affinity::Down});
        }
        SECTION("Escribir la regresa abajo") {
            edit.insert("x");
            CHECK(edit.cursor().affinity == Affinity::Down);
        }
        SECTION("← también") {
            REQUIRE(edit.move_left());
            CHECK(edit.cursor() == TextCursor{10, Affinity::Down});
        }
    }
    SECTION("Con afinidad abajo se dibuja al principio de la fila siguiente") {
        const TextEdit edit = edit_at(text, 11);
        CHECK(edit.cursor_position(kWidth) == RowColumn{1, 0});
    }
    SECTION("Clic después del final de una fila cortada") {
        TextEdit edit = edit_at(text, 0);
        edit.click(0, 20, kWidth);
        CHECK(edit.cursor() == TextCursor{11, Affinity::Up});
        edit.click(1, 20, kWidth); // Última fila: sin corte suave.
        CHECK(edit.cursor() == TextCursor{text.size(), Affinity::Down});
    }
    SECTION("↑/↓ con la columna deseada más allá del final de una fila cortada") {
        // "abc " | "defghijk" con ancho 9 (útil 8).
        TextEdit edit = edit_at("abc defghijk", std::string::npos);
        REQUIRE(edit.cursor_position(9) == RowColumn{1, 8});
        REQUIRE(edit.move_up(9));
        CHECK(edit.cursor() == TextCursor{4, Affinity::Up});
        CHECK(edit.cursor_position(9) == RowColumn{0, 4});
        REQUIRE(edit.move_down(9)); // La columna deseada (8) se conserva.
        CHECK(edit.cursor_position(9) == RowColumn{1, 8});
    }
}

TEST_CASE("text_edit: el cursor en una fila con un espacio colgado", "[text_edit]") {
    // Ancho 5 (útil 4): "xy" | "abcd " (el espacio en la columna reservada) | "efgh".
    const std::string text = "xy\nabcd efgh";
    constexpr int kWidth = 5;
    REQUIRE(chatbot::cli::layout(text, kWidth)[1].hanging);
    SECTION("Fin lo deja antes del espacio, en la columna reservada") {
        TextEdit edit = edit_at(text, 3);
        REQUIRE(edit.move_end(kWidth));
        CHECK(edit.cursor() == TextCursor{7, Affinity::Down});
        CHECK(edit.cursor_position(kWidth) == RowColumn{1, 4});
    }
    SECTION("Clic en el espacio o después: antes del espacio") {
        TextEdit edit = edit_at(text, 0);
        edit.click(1, 4, kWidth);
        CHECK(edit.cursor() == TextCursor{7, Affinity::Down});
        edit.click(1, 30, kWidth);
        CHECK(edit.cursor() == TextCursor{7, Affinity::Down});
        edit.click(1, 3, kWidth);
        CHECK(edit.cursor().offset == 6);
    }
    SECTION("↑/↓ más allá del final: antes del espacio") {
        TextEdit edit = edit_at(text, std::string::npos); // Final de "efgh": columna 4.
        REQUIRE(edit.move_up(kWidth));
        CHECK(edit.cursor() == TextCursor{7, Affinity::Down});
        CHECK(edit.cursor_position(kWidth) == RowColumn{1, 4});
        REQUIRE(edit.move_up(kWidth));
        CHECK(edit.cursor_position(kWidth) == RowColumn{0, 2});
        REQUIRE(edit.move_down(kWidth)); // La columna deseada (4) sigue.
        CHECK(edit.cursor() == TextCursor{7, Affinity::Down});
        REQUIRE(edit.move_down(kWidth));
        CHECK(edit.cursor_position(kWidth) == RowColumn{2, 4});
    }
    SECTION("Justo después del espacio, al principio de la fila siguiente") {
        TextEdit edit = edit_at(text, 7);
        REQUIRE(edit.move_right());
        CHECK(edit.cursor() == TextCursor{8, Affinity::Down});
        CHECK(edit.cursor_position(kWidth) == RowColumn{2, 0});
        // Ni con afinidad arriba se dibuja después del espacio.
        const std::vector<TextRow> rows = chatbot::cli::layout(text, kWidth);
        CHECK(chatbot::cli::to_row_column(text, rows, {8, Affinity::Up}) == RowColumn{2, 0});
        REQUIRE(edit.move_left());
        CHECK(edit.cursor_position(kWidth) == RowColumn{1, 4});
    }
    SECTION("Escribir el espacio que ya no cabe deja el cursor en una fila nueva") {
        TextEdit edit = edit_at("abcd", std::string::npos);
        edit.insert(" ");
        CHECK(edit.rows(kWidth)->size() == 2);
        CHECK(edit.cursor_position(kWidth) == RowColumn{1, 0});
        edit.insert("e");
        CHECK(row_texts(edit.text(), kWidth) == std::vector<std::string>{"abcd ", "e"});
        CHECK(edit.cursor_position(kWidth) == RowColumn{1, 1});
    }
}

TEST_CASE("text_edit: movimiento", "[text_edit]") {
    SECTION("←/→ cruzan cortes suaves y saltos de línea") {
        TextEdit edit = edit_at("ab cd\nef", 0);
        std::vector<std::size_t> seen;
        while (edit.move_right()) {
            seen.push_back(edit.cursor().offset);
        }
        CHECK(seen == std::vector<std::size_t>{1, 2, 3, 4, 5, 6, 7, 8});
        CHECK_FALSE(edit.move_right()); // Al final: no se maneja.
        seen.clear();
        while (edit.move_left()) {
            seen.push_back(edit.cursor().offset);
        }
        CHECK(seen == std::vector<std::size_t>{7, 6, 5, 4, 3, 2, 1, 0});
        CHECK_FALSE(edit.move_left());
    }
    SECTION("↑/↓ conservan la columna deseada a través de filas cortas") {
        // Filas: "abcdefgh" (8), "xy" (2), "abcdefgh" (8), ancho 20.
        TextEdit edit = edit_at("abcdefgh\nxy\nabcdefgh", 6);
        REQUIRE(edit.move_down(20));
        CHECK(edit.cursor_position(20) == RowColumn{1, 2});
        REQUIRE(edit.move_down(20));
        CHECK(edit.cursor_position(20) == RowColumn{2, 6});
        REQUIRE(edit.move_up(20));
        REQUIRE(edit.move_up(20));
        CHECK(edit.cursor_position(20) == RowColumn{0, 6});
    }
    SECTION("↑/↓ con glifos de ancho 2: la mitad va al límite anterior") {
        TextEdit edit = edit_at("abc\n中文字", 3); // Columna 3.
        REQUIRE(edit.move_down(20));
        CHECK(edit.cursor_position(20) == RowColumn{1, 2}); // Antes de 文.
        REQUIRE(edit.move_up(20));
        CHECK(edit.cursor_position(20) == RowColumn{0, 3}); // La deseada sigue en 3.
    }
    SECTION("Una edición o un movimiento horizontal olvidan la columna deseada") {
        TextEdit edit = edit_at("abcdefgh\nxy\nabcdefgh", 6);
        REQUIRE(edit.move_down(20));
        REQUIRE(edit.move_left());
        REQUIRE(edit.move_down(20));
        CHECK(edit.cursor_position(20) == RowColumn{2, 1});
    }
    SECTION("Primera y última fila: al principio y al final, como ftxui::Input") {
        TextEdit edit = edit_at("abc\ndef", 2);
        REQUIRE(edit.move_up(20));
        CHECK(edit.cursor().offset == 0);
        CHECK_FALSE(edit.move_up(20)); // Ya estaba: el evento sigue.
        edit.set_text("abc\ndef", 5);
        REQUIRE(edit.move_down(20));
        CHECK(edit.cursor().offset == 7);
        CHECK_FALSE(edit.move_down(20));
    }
    SECTION("Inicio/Fin van al principio y al final de la fila en pantalla") {
        // "uno dos " | "tres" con ancho 9.
        TextEdit edit = edit_at("uno dos tres\notra", 10); // En "tres".
        REQUIRE(edit.move_home(9));
        CHECK(edit.cursor() == TextCursor{8, Affinity::Down});
        REQUIRE(edit.move_end(9));
        CHECK(edit.cursor() == TextCursor{12, Affinity::Down}); // Antes del "\n".
        edit.set_text("uno dos tres\notra", 2);
        REQUIRE(edit.move_home(9));
        CHECK(edit.cursor().offset == 0);
    }
    SECTION("Ctrl+←/→ por palabra, como ftxui::Input") {
        TextEdit edit = edit_at("¿Qué tal, año_2026?", 0);
        std::vector<std::size_t> right;
        while (edit.move_word_right()) {
            right.push_back(edit.cursor().offset);
        }
        // Bytes: "¿" 0-1, "Qué" 2-5, "tal" 7-9, "año" 12-15, "_" 16, "2026"
        // 17-20, "?" 21. Al final de "¿Qué", "tal", "año", "2026" y del texto.
        CHECK(right == std::vector<std::size_t>{6, 10, 16, 21, 22});
        std::vector<std::size_t> left;
        while (edit.move_word_left()) {
            left.push_back(edit.cursor().offset);
        }
        CHECK(left == std::vector<std::size_t>{17, 12, 7, 2, 0});
    }
    SECTION("PgUp/PgDn mueven el cursor y la vista una altura") {
        std::string text;
        for (int i = 0; i < 30; ++i) {
            text += "fila " + std::to_string(i) + "\n";
        }
        TextEdit edit = edit_at(text, 0);
        REQUIRE(edit.top_row(20, 5) == 0);
        REQUIRE(edit.page(1, 20, 5));
        CHECK(edit.cursor_position(20).row == 5);
        CHECK(edit.top_row(20, 5) == 5);
        REQUIRE(edit.page(-1, 20, 5));
        CHECK(edit.cursor_position(20).row == 0);
        CHECK(edit.top_row(20, 5) == 0);
        CHECK_FALSE(edit.page(-1, 20, 5)); // Ya al principio.
    }
}

TEST_CASE("text_edit: edición", "[text_edit]") {
    SECTION("Un pegado de varias líneas a la mitad, de una vez") {
        TextEdit edit = edit_at("antes  después", 6);
        const std::uint64_t version = edit.version();
        edit.insert("uno\ndos\n");
        CHECK(edit.text() == "antes uno\ndos\n después");
        CHECK(edit.cursor().offset == 6 + 8);
        CHECK(edit.version() == version + 1);
    }
    SECTION("Retroceso borra un glifo de ancho 2 o con combinantes entero") {
        TextEdit edit = edit_at("a中👍🏽e\xCC\x81", std::string::npos);
        REQUIRE(edit.erase_before());
        CHECK(edit.text() == "a中👍🏽");
        REQUIRE(edit.erase_before());
        CHECK(edit.text() == "a中");
        REQUIRE(edit.erase_before());
        CHECK(edit.text() == "a");
        REQUIRE(edit.erase_before());
        CHECK(edit.text().empty());
        CHECK_FALSE(edit.erase_before());
    }
    SECTION("Supr al final no hace nada") {
        TextEdit edit = edit_at("abc", std::string::npos);
        const std::uint64_t version = edit.version();
        CHECK_FALSE(edit.erase_after());
        CHECK(edit.text() == "abc");
        CHECK(edit.version() == version);
        edit.set_text("a中b", 1);
        REQUIRE(edit.erase_after());
        CHECK(edit.text() == "ab");
    }
    SECTION("Sobrescritura (Insert): reemplaza el glifo siguiente, salvo un \\n") {
        TextEdit edit = edit_at("ab\ncd", 0);
        edit.toggle_overwrite();
        edit.type("x");
        CHECK(edit.text() == "xb\ncd");
        edit.type("yz");
        CHECK(edit.text() == "xyz\ncd");
        edit.type("w"); // Antes del "\n": se inserta.
        CHECK(edit.text() == "xyzw\ncd");
    }
    SECTION("set_text deja el cursor al final, con afinidad abajo") {
        TextEdit edit = edit_at("hola mundo feliz", 0);
        REQUIRE(edit.move_end(12));
        edit.set_text("otro texto");
        CHECK(edit.cursor() == TextCursor{10, Affinity::Down});
    }
    SECTION("set_text con un cursor a la mitad de un glifo lo lleva al límite anterior") {
        const TextEdit edit = edit_at("a中b", 2);
        CHECK(edit.cursor().offset == 1);
    }
    SECTION("Una base insertada antes de un combinante sin base se junta con él") {
        TextEdit edit = edit_at("\xCC\x81", 0);
        edit.insert("a"); // Ahora "á": el cursor va después del glifo.
        CHECK(edit.cursor().offset == edit.text().size());
    }
    SECTION("Un combinante insertado después de un combinante sin base") {
        TextEdit edit = edit_at("\xCC\x81", std::string::npos);
        edit.insert("\xCC\x81");
        const chatbot::cli::TextGlyph glyph = chatbot::cli::glyph_at(edit.text(), 0);
        CHECK(glyph.end == 4); // Un solo glifo sin base, con los dos.
        CHECK(glyph.kind == chatbot::cli::GlyphKind::Baseless);
        CHECK(edit.cursor().offset == 4);
    }
}

TEST_CASE("text_edit: vista", "[text_edit]") {
    std::string text;
    for (int i = 0; i < 20; ++i) {
        text += "palabra" + std::to_string(i) + " ";
    }
    // Con ancho 11 (útil 10), una palabra por fila: 20 filas.
    constexpr int kWidth = 11;
    constexpr int kHeight = 4;
    SECTION("El cursor siempre visible al moverse y editar") {
        TextEdit edit = edit_at(text, 0);
        CHECK(edit.top_row(kWidth, kHeight) == 0);
        for (int i = 0; i < 10; ++i) {
            REQUIRE(edit.move_down(kWidth));
        }
        CHECK(edit.top_row(kWidth, kHeight) == 10 - kHeight + 1);
        edit.insert("x");
        const int top = edit.top_row(kWidth, kHeight);
        const int row = edit.cursor_position(kWidth).row;
        CHECK(row >= top);
        CHECK(row < top + kHeight);
    }
    SECTION("La rueda no mueve el cursor y se queda en rango") {
        TextEdit edit = edit_at(text, 0);
        const TextCursor cursor = edit.cursor();
        CHECK(edit.scroll(3, kWidth, kHeight));
        CHECK(edit.top_row(kWidth, kHeight) == 3);
        CHECK(edit.cursor() == cursor);
        CHECK(edit.scroll(100, kWidth, kHeight));
        CHECK(edit.top_row(kWidth, kHeight) == 20 - kHeight);
        CHECK_FALSE(edit.scroll(1, kWidth, kHeight));
        CHECK(edit.scroll(-100, kWidth, kHeight));
        CHECK(edit.top_row(kWidth, kHeight) == 0);
        CHECK_FALSE(edit.scroll(-1, kWidth, kHeight));
    }
    SECTION("Al editar después de la rueda, la vista vuelve al cursor") {
        TextEdit edit = edit_at(text, 0);
        REQUIRE(edit.scroll(10, kWidth, kHeight));
        edit.insert("x");
        CHECK(edit.top_row(kWidth, kHeight) == 0);
    }
    SECTION("Un cambio de ancho conserva el lugar del cursor en el texto y lo deja visible") {
        TextEdit edit = edit_at(text, text.find("palabra12"));
        (void)edit.top_row(kWidth, kHeight);
        REQUIRE(edit.cursor_position(kWidth).row == 12);
        for (const int width : {40, 25, 11, 6}) {
            const int top = edit.top_row(width, kHeight);
            const int row = edit.cursor_position(width).row;
            INFO("ancho " << width);
            CHECK(edit.cursor().offset == text.find("palabra12"));
            CHECK(row >= top);
            CHECK(row < top + kHeight);
        }
    }
}

TEST_CASE("text_edit: la barra con una columna menos nunca tiene menos filas",
          "[text_edit]") {
    // La regla de TextArea: si con el ancho completo no cabe, se distribuye
    // con una columna menos para la barra. Así no oscila: con menos ancho
    // nunca hay menos filas.
    std::mt19937 random(99);
    for (int round = 0; round < 200; ++round) {
        const std::string text = random_text(random, 50);
        for (int width = 2; width < 40; ++width) {
            INFO("ronda " << round << " ancho " << width);
            CHECK(chatbot::cli::layout(text, width - 1).size() >=
                  chatbot::cli::layout(text, width).size());
        }
    }
}

TEST_CASE("text_edit: escribir a la mitad de 256 KiB", "[text_edit][rendimiento]") {
    // El tope de un pegado (sanitize_paste). Ancho 80 y 79 (con barra), y
    // las filas visibles, como en cada cuadro.
    const std::string line = "Una línea de código con acentos: año, niño; también 中文 y 👍🏽.\n";
    std::string text;
    while (text.size() < 256 * 1024) {
        text += line;
    }
    TextEdit edit = edit_at(text, text.size() / 2);
    (void)edit.rows(80);
    (void)edit.rows(79);
    auto best = std::chrono::microseconds::max();
    for (int i = 0; i < 20; ++i) {
        const auto start = std::chrono::steady_clock::now();
        edit.type("x");
        (void)edit.rows(80);
        (void)edit.rows(79);
        (void)edit.top_row(79, 8);
        best = std::min(best, std::chrono::duration_cast<std::chrono::microseconds>(
                                  std::chrono::steady_clock::now() - start));
    }
    REQUIRE(*edit.rows(80) == chatbot::cli::layout(edit.text(), 80));
    const double ms = std::chrono::duration<double, std::milli>(best).count();
    WARN("una tecla con " << text.size() / 1024 << " KiB: " << ms << " ms");
    if (chatbot_test::strict_performance()) {
        CHECK(best < std::chrono::milliseconds{5});
    }
}

TEST_CASE("text_edit: escribir a la mitad de 256 KiB en una sola línea sin espacios",
          "[text_edit][rendimiento]") {
    // El peor caso: base64 o JSON minificado pegado. Sin espacios ni saltos,
    // las filas se cortan por glifo y una letra recorre todos los cortes
    // siguientes de la línea, así que se redistribuye el resto de la línea
    // (lo que el encargo acepta como límite). También sin un solo byte ASCII.
    const std::string units[] = {"QWxhZGRpbjpvcGVuIHNlc2FtZQ+/=", "中文汉字测试，这是一个很长的句子。"};
    for (const std::string& unit : units) {
        std::string text;
        while (text.size() < 256 * 1024) {
            text += unit;
        }
        TextEdit edit = edit_at(text, text.size() / 2);
        (void)edit.rows(80);
        (void)edit.rows(79);
        auto best = std::chrono::microseconds::max();
        for (int i = 0; i < 10; ++i) {
            const auto start = std::chrono::steady_clock::now();
            edit.type("x");
            (void)edit.rows(80);
            (void)edit.rows(79);
            (void)edit.top_row(79, 8);
            best = std::min(best, std::chrono::duration_cast<std::chrono::microseconds>(
                                      std::chrono::steady_clock::now() - start));
        }
        REQUIRE(*edit.rows(79) == chatbot::cli::layout(edit.text(), 79));
        const double ms = std::chrono::duration<double, std::milli>(best).count();
        WARN("una tecla en una línea de " << text.size() / 1024 << " KiB sin espacios: " << ms
                                           << " ms");
        if (chatbot_test::strict_performance()) {
            CHECK(best < std::chrono::milliseconds{25});
        }
    }
}

TEST_CASE("text_edit: la partición en glifos coincide con FTXUI", "[text_edit]") {
    // El modelo parte los glifos por su cuenta: cada fila, dibujada con
    // ftxui::text, debe medir lo mismo (string_width) y dejar cada glifo en la
    // columna que dice el modelo. Piezas: acentos, combinantes (también sin
    // base), emoji con modificador, con ZWJ y con selectores de variación,
    // ancho 2 (CJK, hangul, ancho completo), banderas, U+FFFD escrito, byte
    // inválido, control, "\t", espacio de ancho cero y guion suave.
    constexpr std::string_view kGlyphPieces[] = {
        "a", "ñ", "é", " ", "e\xCC\x81", "\xCC\x81", "中", "文", "👍", "👍🏽",
        "👨\u200D👩\u200D👧", "\u2764\uFE0F", "\u263A\uFE0E", "👍\uFE0F", "\uFFFD", "\xFF", "\x1B",
        "\t", "\uFF71", "\uFF21", "\u1100", "한", "\u200B", "\u00AD", "\U0001F1F2\U0001F1FD",
        "x\u20E3", "中\xCC\x81", "\n",
    };
    std::mt19937 random(31);
    std::uniform_int_distribution<std::size_t> pick(0, std::size(kGlyphPieces) - 1);
    std::uniform_int_distribution<int> lengths(1, 40);
    std::uniform_int_distribution<int> widths(2, 40);
    int wide_with_marks = 0;
    for (int round = 0; round < 400; ++round) {
        std::string text;
        for (int i = lengths(random); i > 0; --i) {
            text += kGlyphPieces[pick(random)];
        }
        const int width = widths(random);
        for (const TextRow& row : chatbot::cli::layout(text, width)) {
            // Lo que dibuja TextArea en cada celda de la fila (glyph_cell).
            struct Placed {
                chatbot::cli::TextGlyph glyph;
                int column;
            };
            std::vector<Placed> placed;
            std::string drawn;
            int column = 0;
            for (std::size_t at = row.start; at < row.end;) {
                const chatbot::cli::TextGlyph glyph = chatbot::cli::glyph_at(text, at);
                placed.push_back({glyph, column});
                for (int cell = 0; cell < glyph.width; ++cell) {
                    drawn += chatbot::cli::glyph_cell(text, glyph, cell);
                }
                column += glyph.width;
                at = glyph.end;
            }
            INFO("ronda " << round << " ancho " << width << " fila [" << drawn << "]");
            CHECK(ftxui::string_width(drawn) == row.width);
            ftxui::Screen screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(row.width + 2),
                                                         ftxui::Dimension::Fixed(1));
            ftxui::Render(screen, ftxui::text(drawn));
            for (const Placed& p : placed) {
                const chatbot::cli::TextGlyph& glyph = p.glyph;
                // Las celdas del glifo según FTXUI, juntas, son las suyas: la
                // misma columna y el mismo ancho.
                std::string ours;
                std::string theirs;
                for (int cell = 0; cell < glyph.width; ++cell) {
                    ours += chatbot::cli::glyph_cell(text, glyph, cell);
                    theirs += screen.CellAt(p.column + cell, 0).character;
                }
                CHECK(theirs == ours);
                // Celda por celda, igual salvo un caso: los combinantes de un
                // glifo ancho. FTXUI los deja en la segunda celda (la que
                // Screen::ToString se salta al imprimir: se pierden) y
                // TextArea, con su base en la primera (se imprimen).
                const bool marks = glyph.base_end < glyph.end;
                if (glyph.width == 2 && marks && glyph.kind == chatbot::cli::GlyphKind::Normal) {
                    ++wide_with_marks;
                    CHECK(screen.CellAt(p.column, 0).character ==
                          text.substr(glyph.start, glyph.base_end - glyph.start));
                    CHECK(screen.CellAt(p.column + 1, 0).character ==
                          text.substr(glyph.base_end, glyph.end - glyph.base_end));
                } else {
                    for (int cell = 0; cell < glyph.width; ++cell) {
                        CHECK(screen.CellAt(p.column + cell, 0).character ==
                              chatbot::cli::glyph_cell(text, glyph, cell));
                    }
                }
            }
        }
    }
    CHECK(wide_with_marks > 0); // El caso de la excepción sí se probó.
}
