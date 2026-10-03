#ifndef CHATBOT_TRANSPORT_H
#define CHATBOT_TRANSPORT_H

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

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
    bool cancelled = false;                 ///< true si el callback abortó la transferencia.
};

/// Callback de streaming: recibe cada trozo crudo del cuerpo y el estado
/// HTTP vigente (las cabeceras llegan antes que el cuerpo, así que el estado
/// ya es conocido en el primer trozo; 0 si no hubo respuesta). Devolver false
/// aborta la transferencia (cancelación, hito 4).
using StreamCallback = std::function<bool(std::string_view chunk, int status)>;

/// Interfaz de transporte: hace el POST y devuelve la respuesta cruda.
/// Diseñada para agregar streaming en el hito 2 sin romper complete().
class Transport {
public:
    virtual ~Transport() = default;
    [[nodiscard]] virtual HttpResponse send(const HttpRequest& request) = 0;

    /// Variante de streaming: entrega cada trozo del cuerpo a on_chunk
    /// conforme llega, junto con el estado HTTP de la respuesta, y además
    /// acumula todo en response.body, igual que send(). Si on_chunk devuelve
    /// false, aborta y devuelve una respuesta con status == 0 y
    /// cancelled == true.
    [[nodiscard]] virtual HttpResponse send_stream(const HttpRequest& request,
                                                   const StreamCallback& on_chunk) = 0;
};

} // namespace chatbot

#endif // CHATBOT_TRANSPORT_H
