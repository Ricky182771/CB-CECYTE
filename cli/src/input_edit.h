#ifndef CHATBOT_CLI_INPUT_EDIT_H
#define CHATBOT_CLI_INPUT_EDIT_H

#include <string>
#include <string_view>

namespace chatbot::cli {

// Edición de la caja de la conversación (sin FTXUI). El cursor es el del
// TextArea de la caja (text_area.h): un índice en bytes de text.

/// Filas en pantalla que la caja crece como máximo (además del tercio de la
/// terminal).
inline constexpr int kMaxInputLines = 8;

/// Alt+Enter: la terminal manda ESC y el Enter, y FTXUI v7.0.3 lo entrega
/// como Event::Special con los dos bytes (ParseESC). Casi todas mandan
/// "\x1B\r"; se acepta también "\x1B\n".
inline constexpr std::string_view kAltEnter = "\x1B\r";
inline constexpr std::string_view kAltEnterLf = "\x1B\n";

/// "\" + Enter: si el carácter justo antes del cursor es "\", lo cambia por
/// "\n" (el cursor queda después del salto) y devuelve true: Enter no envía.
/// Si no, no cambia nada y devuelve false. De "\\" solo se consume la última.
/// Un cursor fuera de rango se acota a [0, text.size()].
bool backslash_newline(std::string& text, int& cursor);

/// Tope del alto de la caja: kMaxInputLines o un tercio de terminal_rows (lo
/// menor), y al menos 1. La caja crece con sus filas en pantalla (con el
/// ajuste de línea) hasta aquí y después se desplaza.
[[nodiscard]] int input_max_rows(int terminal_rows);

/// Placeholder de la caja (con configuración): el más largo que cabe en
/// columns columnas, siempre con cómo hacer un salto de línea si cabe y,
/// después, que "/" abre el menú de comandos.
[[nodiscard]] std::string_view input_placeholder(int columns);

} // namespace chatbot::cli

#endif // CHATBOT_CLI_INPUT_EDIT_H
