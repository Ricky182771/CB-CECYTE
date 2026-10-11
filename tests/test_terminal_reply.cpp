// Respuestas de la terminal que llegan como eventos (terminal_reply.h).

#include "terminal_reply.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

using chatbot::cli::is_terminal_reply;

TEST_CASE("respuestas de la terminal: DCS", "[terminal]") {
    // La que llegó con tmux, tal como la escribió CHAT_DEBUG_KEYS:
    // special [1b 50 31 24 72 20 71 30 20 71 1b 5c].
    const std::string tmux_decrqss{"\x1b\x50\x31\x24\x72\x20\x71\x30\x20\x71\x1b\x5c"};
    CHECK(is_terminal_reply(tmux_decrqss));
    CHECK(is_terminal_reply("\x1BP1$r2 q\x1B\\"));      // DECRQSS de la forma del cursor.
    CHECK(is_terminal_reply("\x1BP>|xterm(390)\x1B\\")); // XTVERSION.
    CHECK(is_terminal_reply("\x1BP!|00000000\x1B\\"));   // DA3.
    CHECK(is_terminal_reply("\x1BP\x1B\\"));
    // Sin el cierre o encimado no es DCS.
    CHECK_FALSE(is_terminal_reply("\x1BP"));   // Alt+Shift+P.
    CHECK_FALSE(is_terminal_reply("\x1BP\\")); // ESC P y luego "\".
    CHECK_FALSE(is_terminal_reply("\x1BP1$r"));
}

TEST_CASE("respuestas de la terminal: DA1 y DA2", "[terminal]") {
    CHECK(is_terminal_reply("\x1B[?62;22c"));
    CHECK(is_terminal_reply("\x1B[?1;2c"));
    CHECK(is_terminal_reply("\x1B[>0;276;0c"));
    CHECK(is_terminal_reply("\x1B[?c"));
    CHECK_FALSE(is_terminal_reply("\x1B[c"));      // Sin ? ni >: no es respuesta.
    CHECK_FALSE(is_terminal_reply("\x1B[?6x;2c")); // Algo que no es número en medio.
    CHECK_FALSE(is_terminal_reply("\x1B[?2004h"));
}

TEST_CASE("respuestas de la terminal: las teclas no cuentan", "[terminal]") {
    // La posición del cursor no cuenta: es igual a Shift+F3.
    CHECK_FALSE(is_terminal_reply("\x1B[1;2R"));
    CHECK_FALSE(is_terminal_reply("\x1B[12;40R"));
    for (const char* key : {"\x1B", "\x1BOP", "\x1B[A", "\x1B[2;2~", "\x1B[200~", "\x1B[201~",
                            "\x1B\r", "\x1B\x7F", "\x1B" "c", "\x1B\\", "\x10", "a", ""}) {
        CAPTURE(std::string{key});
        CHECK_FALSE(is_terminal_reply(key));
    }
}
