#ifndef CHATBOT_TESTS_SYMBOLS_CHECK_HPP
#define CHATBOT_TESTS_SYMBOLS_CHECK_HPP

// Compara el mismo dibujo con símbolos Unicode y Sencillos: misma pantalla,
// celda por celda, salvo los símbolos de glyph_table(), que cambian por su
// respaldo de una columna con el mismo estilo (así se ven igual de claros
// con cualquier tema y con NO_COLOR). Solo para dibujos sin los respaldos
// de otro ancho (las tareas de markdown).

#include "glyphs.h"
#include "theme.h"

#include <catch2/catch_test_macros.hpp>

#include <ftxui/screen/screen.hpp>

#include <optional>
#include <string>
#include <vector>

namespace chatbot_test {

/// Las paletas con que se prueba: cada tema con cada fondo (el tema "De la
/// terminal" con fondo transparente es el de NO_COLOR).
inline std::vector<chatbot::cli::Palette> palettes_with(chatbot::cli::Symbols symbols) {
    std::vector<chatbot::cli::Palette> out;
    for (const chatbot::cli::Theme& theme : chatbot::cli::themes()) {
        for (const auto background : {chatbot::cli::BackgroundMode::FromTheme,
                                      chatbot::cli::BackgroundMode::Transparent}) {
            out.emplace_back(theme, background, symbols);
        }
    }
    return out;
}

inline bool in_glyph_table(const std::string& character) {
    for (const chatbot::cli::GlyphInfo& info : chatbot::cli::glyph_table()) {
        if (info.unicode == character) {
            return true;
        }
    }
    return false;
}

/// Cuántas celdas de unicode llevan un símbolo de la tabla.
inline int table_glyphs_in(const ftxui::Screen& screen) {
    int count = 0;
    for (int y = 0; y < screen.dimy(); ++y) {
        for (int x = 0; x < screen.dimx(); ++x) {
            count += in_glyph_table(screen.CellAt(x, y).character) ? 1 : 0;
        }
    }
    return count;
}

/// simple es unicode con cada símbolo de la tabla cambiado por su respaldo,
/// en la misma celda y con el mismo estilo; ningún símbolo de la tabla queda.
inline void check_simple_matches(const ftxui::Screen& unicode, const ftxui::Screen& simple) {
    REQUIRE(unicode.dimx() == simple.dimx());
    REQUIRE(unicode.dimy() == simple.dimy());
    std::vector<std::string> problems;
    for (int y = 0; y < unicode.dimy(); ++y) {
        for (int x = 0; x < unicode.dimx(); ++x) {
            const ftxui::Cell& u = unicode.CellAt(x, y);
            const ftxui::Cell& s = simple.CellAt(x, y);
            const std::optional<std::string_view> replacement =
                chatbot::cli::simple_for(u.character);
            const std::string expected =
                replacement.has_value() ? std::string{*replacement} : u.character;
            const std::string where = "(" + std::to_string(x) + ", " + std::to_string(y) + ")";
            if (s.character != expected) {
                problems.push_back(where + ": \"" + u.character + "\" -> \"" + s.character +
                                   "\", se esperaba \"" + expected + "\"");
            }
            if (in_glyph_table(s.character)) {
                problems.push_back(where + ": quedó \"" + s.character + "\"");
            }
            if (u.foreground_color != s.foreground_color ||
                u.background_color != s.background_color || u.bold != s.bold ||
                u.dim != s.dim || u.inverted != s.inverted || u.underlined != s.underlined) {
                problems.push_back(where + ": cambió el estilo de \"" + u.character + "\"");
            }
        }
    }
    for (const std::string& problem : problems) {
        UNSCOPED_INFO(problem);
    }
    CHECK(problems.empty());
}

} // namespace chatbot_test

#endif // CHATBOT_TESTS_SYMBOLS_CHECK_HPP
