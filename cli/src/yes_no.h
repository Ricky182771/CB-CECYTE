#ifndef CHATBOT_CLI_YES_NO_H
#define CHATBOT_CLI_YES_NO_H

#include <string_view>

namespace chatbot::cli {

/// Lo que contesta un evento a una pregunta (s/n).
enum class YesNo { Yes, No, Cancel, Ignore };

/// La regla de toda pregunta (s/n) (F2: descartar y las de Addons; la barra
/// lateral: borrar una conversación): s/S sí, n/N no y Esc cancela (quien
/// pregunta dice qué no se hizo). Lo demás se ignora: el ratón, otras teclas,
/// Enter y el Event::Custom que manda cada post de main.cpp. character: el
/// texto del evento si es un carácter; si no, vacío.
[[nodiscard]] constexpr YesNo yes_no_answer(bool escape, std::string_view character) {
    if (escape) {
        return YesNo::Cancel;
    }
    if (character == "s" || character == "S") {
        return YesNo::Yes;
    }
    if (character == "n" || character == "N") {
        return YesNo::No;
    }
    return YesNo::Ignore;
}

} // namespace chatbot::cli

#endif // CHATBOT_CLI_YES_NO_H
