#ifndef CHATBOT_CURL_TRANSPORT_H
#define CHATBOT_CURL_TRANSPORT_H

#include "chatbot/transport.h"

#include <memory>

namespace chatbot {

/// Implementación real de Transport sobre libcurl (secciones 6, 8 y 10).
/// Todos los recursos de curl viven bajo RAII y los tipos de curl no
/// aparecen en este header público.
class CurlTransport final : public Transport {
public:
    CurlTransport();
    ~CurlTransport() override;

    CurlTransport(const CurlTransport&) = delete;
    CurlTransport& operator=(const CurlTransport&) = delete;
    CurlTransport(CurlTransport&&) = delete;
    CurlTransport& operator=(CurlTransport&&) = delete;

    [[nodiscard]] HttpResponse send(const HttpRequest& request) override;

    [[nodiscard]] HttpResponse send_stream(const HttpRequest& request,
                                           const StreamCallback& on_chunk) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    /// Configura y ejecuta la petición; stream no nulo activa el callback
    /// de streaming (compartido por send y send_stream).
    HttpResponse perform(const HttpRequest& request, const StreamCallback* stream);
};

} // namespace chatbot

#endif // CHATBOT_CURL_TRANSPORT_H
