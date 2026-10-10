#ifndef CHATBOT_CLI_PASTE_H
#define CHATBOT_CLI_PASTE_H

#include <cstddef>
#include <string>
#include <string_view>

namespace chatbot::cli {

// Pegado de texto (sin FTXUI ni Win32): el saneado de lo pegado.

/// Tope de un pegado, en bytes UTF-8 ya saneados.
inline constexpr std::size_t kMaxPasteBytes = 256 * 1024;

/// Texto pegado listo para insertar.
struct SanitizedPaste {
    std::string text;
    bool truncated = false; ///< Se recortó a kMaxPasteBytes.
};

/// Limpia el texto pegado:
/// - "\r\n" y "\r" → "\n"; "\t" → 4 espacios;
/// - fuera los demás controles C0, DEL (0x7F) y los C1 (U+0080 a U+009F);
/// - cada byte de UTF-8 inválido → U+FFFD;
/// - multiline == false: fuera los "\n" del principio y del final, y los de
///   en medio → un espacio (una key copiada de una página con salto final);
/// - recortado a kMaxPasteBytes sin partir un carácter (truncated = true).
[[nodiscard]] SanitizedPaste sanitize_paste(std::string_view text, bool multiline);

} // namespace chatbot::cli

#endif // CHATBOT_CLI_PASTE_H
