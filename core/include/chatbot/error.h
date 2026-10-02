#ifndef CHATBOT_ERROR_H
#define CHATBOT_ERROR_H

#include <chrono>
#include <optional>
#include <string>

namespace chatbot {

/// Clasificación de los errores del núcleo.
enum class ErrorKind {
    Config,        ///< Configuración ausente o inválida (key, modelo, etc.).
    Auth,          ///< 401 / 403.
    ModelNotFound, ///< 404 / 410.
    RateLimited,   ///< 429.
    Server,        ///< 5xx.
    Network,       ///< Fallo de red de curl.
    Timeout,       ///< Curl excedió el tiempo límite.
    BadResponse,   ///< Cuerpo inesperado o no interpretable.
    Cancelled,     ///< Cancelado por el usuario (se usará en el hito 4).
};

/// Error tipado que nunca viaja como excepción por la API pública.
struct ChatError {
    ErrorKind kind = ErrorKind::Network;
    int http_status = 0; ///< 0 si no hubo respuesta HTTP.
    std::string message;
    std::optional<std::chrono::seconds> retry_after;
};

/// Etiqueta legible (en español) para un tipo de error.
const char* error_kind_label(ErrorKind kind);

} // namespace chatbot

#endif // CHATBOT_ERROR_H
