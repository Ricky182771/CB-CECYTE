// Símbolos de la interfaz (glyphs.h): detección automática, modo guardado,
// tabla de reemplazos.

#include "glyphs.h"

#include "chatbot/platform.h"
#include "chatbot/utf8.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

using chatbot::Os;
using chatbot::cli::Glyph;
using chatbot::cli::SymbolMode;
using chatbot::cli::Symbols;

std::string encode(std::uint32_t code_point) {
    std::string out;
    if (code_point < 0x80) {
        out += static_cast<char>(code_point);
    } else if (code_point < 0x800) {
        out += static_cast<char>(0xC0 | (code_point >> 6));
        out += static_cast<char>(0x80 | (code_point & 0x3F));
    } else if (code_point < 0x10000) {
        out += static_cast<char>(0xE0 | (code_point >> 12));
        out += static_cast<char>(0x80 | ((code_point >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (code_point & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (code_point >> 18));
        out += static_cast<char>(0x80 | ((code_point >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((code_point >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (code_point & 0x3F));
    }
    return out;
}

/// Las letras y signos del español: se quedan siempre.
bool is_spanish(std::string_view character) {
    static const std::set<std::string_view> kSpanish{"á", "é", "í", "ó", "ú", "Á", "É", "Í",
                                                     "Ó", "Ú", "ñ", "Ñ", "ü", "Ü", "¿", "¡"};
    return kSpanish.contains(character);
}

bool is_kept(std::string_view character) {
    const auto kept = chatbot::cli::kept_glyphs();
    return std::find(kept.begin(), kept.end(), character) != kept.end();
}

/// true si text es ASCII o solo lleva símbolos de kept_glyphs().
bool ascii_or_kept(std::string_view text) {
    std::size_t i = 0;
    while (i < text.size()) {
        const std::optional<std::uint32_t> code = chatbot::utf8::next_code_point(text, i);
        if (!code.has_value()) {
            return false;
        }
        if (*code >= 0x80 && !is_kept(encode(*code))) {
            return false;
        }
    }
    return true;
}

} // namespace

TEST_CASE("símbolos: detección automática", "[simbolos]") {
    using chatbot::cli::detect_symbols;
    // Windows 10 21H2 (las PCs del plantel), en conhost: Sencillos.
    CHECK(detect_symbols(Os::Windows, 19044, false, false) == Symbols::Simple);
    CHECK(detect_symbols(Os::Windows, 21999, false, false) == Symbols::Simple);
    // Windows Terminal en Windows 10 define WT_SESSION: Unicode.
    CHECK(detect_symbols(Os::Windows, 19044, true, false) == Symbols::Unicode);
    // mintty, VS Code y otras definen TERM_PROGRAM: Unicode.
    CHECK(detect_symbols(Os::Windows, 19044, false, true) == Symbols::Unicode);
    CHECK(detect_symbols(Os::Windows, 19044, true, true) == Symbols::Unicode);
    CHECK(detect_symbols(Os::Windows, std::nullopt, false, true) == Symbols::Unicode);
    // Windows 11 (22000 o más): conhost busca el símbolo en otra fuente.
    CHECK(detect_symbols(Os::Windows, 22000, false, false) == Symbols::Unicode);
    CHECK(detect_symbols(Os::Windows, 26100, false, false) == Symbols::Unicode);
    CHECK(detect_symbols(Os::Windows, 26100, true, true) == Symbols::Unicode);
    // Sin el build (no debería pasar): lo que se lee en todas.
    CHECK(detect_symbols(Os::Windows, std::nullopt, false, false) == Symbols::Simple);
    // Linux y Termux: siempre Unicode, con o sin las variables (WSL hereda
    // WT_SESSION).
    for (const bool wt_session : {false, true}) {
        for (const bool term_program : {false, true}) {
            CHECK(detect_symbols(Os::Posix, std::nullopt, wt_session, term_program) ==
                  Symbols::Unicode);
            CHECK(detect_symbols(Os::Posix, 19044, wt_session, term_program) ==
                  Symbols::Unicode);
        }
    }
}

TEST_CASE("símbolos: el modo elegido gana a la detección", "[simbolos]") {
    for (const Symbols detected : {Symbols::Unicode, Symbols::Simple}) {
        CHECK(chatbot::cli::resolve_symbols(SymbolMode::Auto, detected) == detected);
        CHECK(chatbot::cli::resolve_symbols(SymbolMode::Unicode, detected) == Symbols::Unicode);
        CHECK(chatbot::cli::resolve_symbols(SymbolMode::Simple, detected) == Symbols::Simple);
    }
}

TEST_CASE("símbolos: ids de config.json", "[simbolos][config]") {
    for (const SymbolMode mode : {SymbolMode::Auto, SymbolMode::Unicode, SymbolMode::Simple}) {
        CHECK(chatbot::cli::parse_symbol_mode(chatbot::cli::symbol_mode_id(mode)) == mode);
    }
    CHECK(chatbot::cli::symbol_mode_id(SymbolMode::Auto) == "auto");
    CHECK(chatbot::cli::symbol_mode_id(SymbolMode::Unicode) == "unicode");
    CHECK(chatbot::cli::symbol_mode_id(SymbolMode::Simple) == "simple");
    CHECK_FALSE(chatbot::cli::parse_symbol_mode("").has_value());
    CHECK_FALSE(chatbot::cli::parse_symbol_mode("Simple").has_value());
    CHECK_FALSE(chatbot::cli::parse_symbol_mode("ascii").has_value());
}

TEST_CASE("símbolos: la tabla", "[simbolos]") {
    const auto table = chatbot::cli::glyph_table();
    REQUIRE_FALSE(table.empty());
    std::set<std::string_view> seen;
    for (std::size_t i = 0; i < table.size(); ++i) {
        const chatbot::cli::GlyphInfo& info = table[i];
        INFO(info.unicode);
        CHECK(static_cast<std::size_t>(info.glyph) == i);
        CHECK(chatbot::cli::glyph(info.glyph, Symbols::Unicode) == info.unicode);
        CHECK(chatbot::cli::glyph(info.glyph, Symbols::Simple) == info.simple);
        CHECK(chatbot::cli::simple_for(info.unicode) == info.simple);
        CHECK(seen.insert(info.unicode).second); // Sin repetidos.
        CHECK(info.unicode != info.simple);
        // Un reemplazo nunca es otro símbolo que falte: ASCII o de los que
        // se quedan.
        CHECK(ascii_or_kept(info.simple));
        CHECK_FALSE(is_kept(info.unicode));
    }
    CHECK_FALSE(chatbot::cli::simple_for("─").has_value());
    CHECK_FALSE(chatbot::cli::simple_for("a").has_value());
    for (const std::string_view kept : chatbot::cli::kept_glyphs()) {
        INFO(kept);
        CHECK_FALSE(is_spanish(kept));
        CHECK(ascii_or_kept(kept));
    }
}
