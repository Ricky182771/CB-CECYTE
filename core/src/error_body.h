#ifndef CHATBOT_SRC_ERROR_BODY_H
#define CHATBOT_SRC_ERROR_BODY_H

#include "chatbot/error.h"

#include <string>

namespace chatbot {

/// Clasifica un estado HTTP no exitoso (sección 8). También se usa para el
/// error.code numérico de los errores que llegan dentro del flujo SSE.
[[nodiscard]] ErrorKind kind_for_http_status(int status);

/// Corta textos demasiado largos (sección 8).
/// Header interno (src/); no forma parte de la API pública.
[[nodiscard]] std::string truncate_message(const std::string& text);

/// Intenta extraer el mensaje de error del cuerpo (error.message o detail,
/// sección 8), ya truncado; si no hay, devuelve el texto alternativo.
/// No lanza excepciones.
[[nodiscard]] std::string extract_error_message(const std::string& body,
                                                const std::string& fallback);

} // namespace chatbot

#endif // CHATBOT_SRC_ERROR_BODY_H
