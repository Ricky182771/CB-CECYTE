#include "glyphs.h"

#include <array>
#include <cstddef>

namespace chatbot::cli {

namespace {

// Los que faltan en Consolas o en Lucida Console (ver el README, "Símbolos").
// Un respaldo de otro ancho lo dice; donde se usa, el layout mide el texto.
constexpr std::array<GlyphInfo, 15> kGlyphs{{
    {Glyph::Gear, "⚙", "*"},
    {Glyph::Check, "✓", "√"},
    {Glyph::Warning, "⚠", "!"},
    {Glyph::TaskDone, "☑", "[x]"}, // 3 columnas: la sangría de la lista se mide.
    {Glyph::TaskOpen, "☐", "[ ]"},
    {Glyph::DropdownOpen, "▴", "^"},
    {Glyph::DropdownClosed, "▾", "v"},
    {Glyph::HeadingMark, "▍", "▌"},
    {Glyph::RoundedTopLeft, "╭", "┌"},
    {Glyph::RoundedTopRight, "╮", "┐"},
    {Glyph::RoundedBottomLeft, "╰", "└"},
    {Glyph::RoundedBottomRight, "╯", "┘"},
    {Glyph::ScrollThumb, "┃", "█"},
    {Glyph::ScrollThumbTop, "╹", "▀"},
    {Glyph::ScrollThumbBottom, "╻", "▄"},
}};

constexpr std::array<std::string_view, 28> kKept{
    // Texto y avisos.
    "·", "«", "»", "—", "…", "→", "↑", "↓",
    // Listas: viñetas, elegido y la key enmascarada de FTXUI (password).
    "•", "◦", "▪", "●",
    // Bordes y separadores rectos (los de FTXUI y los de las tablas).
    "─", "│", "┌", "┐", "└", "┘", "├", "┤", "┬", "┴", "┼",
    // Respaldos que no son ASCII.
    "√", "▌", "█", "▀", "▄",
};

/// La tabla está en el orden de Glyph: glyph() la indexa directo.
constexpr bool table_in_order() {
    for (std::size_t i = 0; i < kGlyphs.size(); ++i) {
        if (static_cast<std::size_t>(kGlyphs[i].glyph) != i) {
            return false;
        }
    }
    return true;
}
static_assert(table_in_order());

} // namespace

std::string_view symbol_mode_id(SymbolMode mode) {
    switch (mode) {
    case SymbolMode::Unicode:
        return "unicode";
    case SymbolMode::Simple:
        return "simple";
    case SymbolMode::Auto:
        break;
    }
    return "auto";
}

std::optional<SymbolMode> parse_symbol_mode(std::string_view id) {
    for (const SymbolMode mode : {SymbolMode::Auto, SymbolMode::Unicode, SymbolMode::Simple}) {
        if (id == symbol_mode_id(mode)) {
            return mode;
        }
    }
    return std::nullopt;
}

Symbols detect_symbols(Os os, std::optional<std::uint32_t> windows_build, bool wt_session,
                       bool term_program) {
    if (os != Os::Windows || wt_session || term_program) {
        return Symbols::Unicode;
    }
    // Sin el build (no debería pasar), lo seguro: Sencillos se lee en todas.
    if (windows_build.has_value() && *windows_build >= kFirstWindows11Build) {
        return Symbols::Unicode;
    }
    return Symbols::Simple;
}

Symbols resolve_symbols(SymbolMode mode, Symbols detected) {
    switch (mode) {
    case SymbolMode::Unicode:
        return Symbols::Unicode;
    case SymbolMode::Simple:
        return Symbols::Simple;
    case SymbolMode::Auto:
        break;
    }
    return detected;
}

std::span<const GlyphInfo> glyph_table() { return kGlyphs; }

std::string_view glyph(Glyph which, Symbols symbols) {
    const GlyphInfo& info = kGlyphs[static_cast<std::size_t>(which)];
    return symbols == Symbols::Simple ? info.simple : info.unicode;
}

std::optional<std::string_view> simple_for(std::string_view unicode) {
    for (const GlyphInfo& info : kGlyphs) {
        if (info.unicode == unicode) {
            return info.simple;
        }
    }
    return std::nullopt;
}

std::span<const std::string_view> kept_glyphs() { return kKept; }

} // namespace chatbot::cli
