#ifndef CHATBOT_CLI_GLYPHS_H
#define CHATBOT_CLI_GLYPHS_H

#include "chatbot/platform.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace chatbot::cli {

// Símbolos de la interfaz. La consola de Windows 10 (conhost) dibuja con una
// sola fuente (Consolas o Lucida Console) y, si el símbolo no está en ella,
// sale un cuadrito. En modo Sencillos se reemplazan los símbolos que faltan
// en alguna de las dos fuentes; los demás (y los acentos y la ñ) se quedan.
// El texto que viene del modelo, del usuario o de un archivo nunca se toca.
//
// Regla: ningún símbolo de glyph_table() se escribe directo en cli/src fuera
// de glyphs.cpp, y ningún carácter fuera de ASCII que no sea una letra del
// español ni de kept_glyphs() aparece en el código de dibujo (lo revisa
// tests/test_glyphs.cpp).

/// Juego de símbolos ya resuelto.
enum class Symbols : std::uint8_t {
    Unicode, ///< Los de siempre.
    Simple,  ///< Sin los que faltan en Consolas o Lucida Console.
};

/// "Símbolos: Automático / Unicode / Sencillos". En config.json:
/// "appearance": {"symbols": "auto" | "unicode" | "simple"}.
enum class SymbolMode : std::uint8_t { Auto, Unicode, Simple };

[[nodiscard]] std::string_view symbol_mode_id(SymbolMode mode);
[[nodiscard]] std::optional<SymbolMode> parse_symbol_mode(std::string_view id);

/// Primer build de Windows 11. Desde ahí, conhost busca en otra fuente el
/// símbolo que no tiene la suya.
inline constexpr std::uint32_t kFirstWindows11Build = 22000;

/// Detección automática (función pura): Sencillos solo en Windows, con un
/// build menor que kFirstWindows11Build (o desconocido), sin WT_SESSION
/// (Windows Terminal la define; conhost, no) y sin TERM_PROGRAM (la definen
/// mintty, la terminal de VS Code y otras, que dibujan con sus fuentes). En
/// todo lo demás, Unicode.
[[nodiscard]] Symbols detect_symbols(Os os, std::optional<std::uint32_t> windows_build,
                                     bool wt_session, bool term_program);

/// El juego que se usa: el elegido, o detected con Automático.
[[nodiscard]] Symbols resolve_symbols(SymbolMode mode, Symbols detected);

/// Símbolos que se reemplazan en modo Sencillos.
enum class Glyph : std::uint8_t {
    Gear,          ///< "⚙ Configuración" (barra lateral).
    Check,         ///< "[✓ Copiado]" (botones de los bloques).
    Warning,       ///< "⚠ #1 incompleto" (avisos de los botones).
    TaskDone,      ///< Tarea hecha en una lista de markdown.
    TaskOpen,      ///< Tarea pendiente.
    DropdownOpen,  ///< Lista de proveedores abierta (configuración).
    DropdownClosed,
    HeadingMark,   ///< Marca de los títulos H3-H6.
    ChosenMark,    ///< Elegido en las listas (list_style.h).
    BulletLevel2,  ///< Viñeta del segundo nivel de una lista de markdown.
    BulletLevel3,  ///< Viñeta del tercer nivel (el primero, "•", se queda).
    // Los de FTXUI: los cambia Palette (border_style, vscroll).
    RoundedTopLeft, ///< borderRounded y window.
    RoundedTopRight,
    RoundedBottomLeft,
    RoundedBottomRight,
    ScrollThumb,       ///< vscroll_indicator: celda completa de la barra.
    ScrollThumbTop,    ///< Solo la mitad de arriba.
    ScrollThumbBottom, ///< Solo la mitad de abajo.
};

/// Un símbolo y su respaldo, del mismo ancho en celdas salvo los que dicen
/// otra cosa en glyphs.cpp (y el layout que los usa mide el texto).
struct GlyphInfo {
    Glyph glyph;
    std::string_view unicode;
    std::string_view simple;
};

/// Todos los reemplazos, en el orden de Glyph.
[[nodiscard]] std::span<const GlyphInfo> glyph_table();

/// El símbolo en ese juego.
[[nodiscard]] std::string_view glyph(Glyph which, Symbols symbols);

/// El respaldo de un símbolo de la tabla escrito por FTXUI (una celda), o
/// nullopt si no está en la tabla.
[[nodiscard]] std::optional<std::string_view> simple_for(std::string_view unicode);

/// Símbolos fuera de ASCII que la interfaz dibuja tal cual también en modo
/// Sencillos, porque están en Consolas y en Lucida Console (y los que usa
/// FTXUI por su cuenta, como los bordes rectos). Los respaldos de la tabla
/// salen de aquí o de ASCII.
[[nodiscard]] std::span<const std::string_view> kept_glyphs();

} // namespace chatbot::cli

#endif // CHATBOT_CLI_GLYPHS_H
