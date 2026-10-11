#ifndef CHATBOT_CLI_TERMINAL_REPLY_H
#define CHATBOT_CLI_TERMINAL_REPLY_H

#include <string_view>

namespace chatbot::cli {

// Respuestas de la terminal a las consultas que hace FTXUI al arrancar.
// FTXUI 7.0.3 consume las que sabe leer, pero una que no entiende llega a la
// interfaz como Event::Special: por ejemplo, la de tmux a DECRQSS de la
// forma del cursor, "ESC P 1 $ r ␠ q 0 ␠ q ESC \" (visto con
// CHAT_DEBUG_KEYS). No son teclas: no deben borrar el aviso de la línea de
// estado ni llegar a los componentes.

/// true si input (la secuencia de un Event::Special) es una respuesta de la
/// terminal y no una tecla:
/// - DCS: ESC P … ESC \ (DECRQSS, XTVERSION, DA3).
/// - DA1 y DA2: ESC [ ? … c y ESC [ > … c (solo dígitos y ';' en medio).
/// La posición del cursor (ESC [ fila ; columna R) no cuenta: es igual a
/// Shift+F3 y otras teclas con modificadores (ESC [ 1 ; 2 R).
[[nodiscard]] bool is_terminal_reply(std::string_view input);

} // namespace chatbot::cli

#endif // CHATBOT_CLI_TERMINAL_REPLY_H
