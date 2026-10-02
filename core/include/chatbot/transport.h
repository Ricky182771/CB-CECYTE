#ifndef CHATBOT_TRANSPORT_H
#define CHATBOT_TRANSPORT_H

#include <chrono>
#include <optional>
#include <string>

namespace chatbot {

/// Petición HTTP lista para enviarse.
struct HttpRequest {
    std::string url;     ///< URL completa del endpoint.
    std::string body;    ///< Cuerpo JSON de la petición.
    std::string api_key; ///< Se usa solo para armar la cabecera Authorization; nunca se registra.
    /// Timeout total. 0 significa sin límite (semántica de curl).
    std::chrono::milliseconds timeout{120000};
};

/// Respuesta HTTP cruda, antes de interpretarla.
struct HttpResponse {
    int status = 0; ///< Código HTTP; 0 si no hubo respuesta (fallo de red).
    std::string body;
    std::optional<std::string> retry_after; ///< Valor crudo de Retry-After, si llegó.
    bool timed_out = false;                 ///< true si curl agotó el tiempo de espera.
    std::string error;                      ///< Descripción del fallo de red (solo si status == 0).
};

/// Interfaz de transporte: hace el POST y devuelve la respuesta cruda.
/// Diseñada para agregar streaming en el hito 2 sin romper complete().
class Transport {
public:
    virtual ~Transport() = default;
    [[nodiscard]] virtual HttpResponse send(const HttpRequest& request) = 0;
};

} // namespace chatbot

#endif // CHATBOT_TRANSPORT_H
