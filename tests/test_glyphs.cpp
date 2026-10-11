// Símbolos de la interfaz (glyphs.h): detección automática, modo guardado,
// tabla de reemplazos y que ningún símbolo se escriba fuera de la tabla.

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

/// Quita los comentarios de un archivo de C++ (// y /* */) y deja los
/// literales (también los raw strings), con los saltos de línea en su lugar
/// para que los números de línea sigan valiendo.
std::string strip_comments(std::string_view source) {
    std::string out;
    std::size_t i = 0;
    const auto copy_until = [&](std::string_view end) {
        const std::size_t found = source.find(end, i);
        const std::size_t stop = found == std::string_view::npos ? source.size() : found + end.size();
        out.append(source.substr(i, stop - i));
        i = stop;
    };
    while (i < source.size()) {
        const std::string_view rest = source.substr(i);
        if (rest.starts_with("//")) {
            i = std::min(source.size(), source.find('\n', i));
            continue;
        }
        if (rest.starts_with("/*")) {
            const std::size_t end = source.find("*/", i + 2);
            const std::size_t stop = end == std::string_view::npos ? source.size() : end + 2;
            out.append(static_cast<std::size_t>(
                           std::count(source.begin() + static_cast<std::ptrdiff_t>(i),
                                      source.begin() + static_cast<std::ptrdiff_t>(stop), '\n')),
                       '\n');
            i = stop;
            continue;
        }
        if (rest.starts_with("R\"")) {
            const std::size_t open = source.find('(', i);
            const std::string delimiter{source.substr(i + 2, open - i - 2)};
            copy_until(")" + delimiter + "\"");
            continue;
        }
        const char c = source[i];
        if (c == '"' || c == '\'') {
            out += c;
            ++i;
            while (i < source.size() && source[i] != c && source[i] != '\n') {
                if (source[i] == '\\' && i + 1 < source.size()) {
                    out += source[i++];
                }
                out += source[i++];
            }
            if (i < source.size()) {
                out += source[i++];
            }
            continue;
        }
        out += c;
        ++i;
    }
    return out;
}

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

bool in_table(std::string_view character) {
    for (const chatbot::cli::GlyphInfo& info : chatbot::cli::glyph_table()) {
        if (info.unicode == character) {
            return true;
        }
    }
    return false;
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

TEST_CASE("símbolos: ninguno se escribe fuera de la tabla en cli/src", "[simbolos]") {
    namespace fs = std::filesystem;
    const fs::path dir{CHATBOT_CLI_SOURCE_DIR};
    REQUIRE(fs::is_directory(dir));
    std::vector<std::string> problems;
    std::size_t files = 0;
    for (const fs::directory_entry& entry : fs::directory_iterator(dir)) {
        const fs::path& path = entry.path();
        if (path.extension() != ".cpp" && path.extension() != ".h") {
            continue;
        }
        ++files;
        std::ifstream file{path, std::ios::binary};
        const std::string source{std::istreambuf_iterator<char>{file},
                                 std::istreambuf_iterator<char>{}};
        const std::string code = strip_comments(source);
        const bool is_table = path.filename() == "glyphs.cpp";
        int line = 1;
        std::size_t i = 0;
        while (i < code.size()) {
            if (code[i] == '\n') {
                ++line;
            }
            const std::size_t start = i;
            const std::optional<std::uint32_t> code_point = chatbot::utf8::next_code_point(code, i);
            if (!code_point.has_value()) {
                problems.push_back(path.filename().string() + ":" + std::to_string(line) +
                                   ": UTF-8 inválido");
                ++i;
                continue;
            }
            if (*code_point < 0x80) {
                continue;
            }
            const std::string character = code.substr(start, i - start);
            if (is_spanish(character) || is_kept(character) || (is_table && in_table(character))) {
                continue;
            }
            problems.push_back(path.filename().string() + ":" + std::to_string(line) + ": " +
                               character +
                               (in_table(character) ? " (usa glyph() o Palette::glyph())"
                                                    : " (agrégalo a glyphs.cpp)"));
        }
    }
    CHECK(files > 50);
    for (const std::string& problem : problems) {
        UNSCOPED_INFO(problem);
    }
    CHECK(problems.empty());
}

TEST_CASE("símbolos: el detector de comentarios del recorrido", "[simbolos]") {
    // Los comentarios se quitan; los literales (también con // adentro) no.
    CHECK(strip_comments("a // ⚠ b\nc") == "a \nc");
    CHECK(strip_comments("a /* ⚠\n */ b") == "a \n b");
    CHECK(strip_comments("\"https://x\" ⚠") == "\"https://x\" ⚠");
    CHECK(strip_comments("\"a\\\"// b\" c") == "\"a\\\"// b\" c");
    CHECK(strip_comments("R\"(// ⚠)\" // x") == "R\"(// ⚠)\" ");
    CHECK(strip_comments("'/' // ⚠") == "'/' ");
}
