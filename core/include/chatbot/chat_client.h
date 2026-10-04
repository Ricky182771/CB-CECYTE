#ifndef CHATBOT_CHAT_CLIENT_H
#define CHATBOT_CHAT_CLIENT_H

#include "chatbot/cancel_token.h"
#include "chatbot/completion_info.h"
#include "chatbot/config.h"
#include "chatbot/result.h"
#include "chatbot/sleeper.h"
#include "chatbot/transport.h"
#include "chatbot/types.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace chatbot {

/// Callback de streaming: recibe cada incremento de texto del asistente.
/// Devolver false cancela la petición (se devuelve ErrorKind::Cancelled).
using StreamDeltaCallback = std::function<bool(std::string_view delta)>;

/// Cliente del hito 1: petición simple sin streaming, con reintentos.
/// Las excepciones internas nunca cruzan la API pública (sección 6).
class ChatClient {
public:
    /// Toma posesión del transporte. Usa RealSleeper salvo que se inyecte
    /// otro (para que las pruebas no esperen de verdad).
    ChatClient(Config config, std::unique_ptr<Transport> transport);
    ChatClient(Config config, std::unique_ptr<Transport> transport,
               std::unique_ptr<Sleeper> sleeper);
    ~ChatClient();

    ChatClient(const ChatClient&) = delete;
    ChatClient& operator=(const ChatClient&) = delete;
    ChatClient(ChatClient&&) = delete;
    ChatClient& operator=(ChatClient&&) = delete;

    /// Envía la conversación completa y devuelve el contenido de la respuesta
    /// del asistente (choices[0].message.content). Nunca lanza excepciones.
    /// Con cancel: se revisa antes de cada intento, viaja al transporte e
    /// interrumpe la espera de reintento; si se cancela, devuelve
    /// ErrorKind::Cancelled sin reintentar. El token debe vivir toda la llamada.
    [[nodiscard]] Result<std::string> complete(const std::vector<Message>& messages,
                                               const CancelToken* cancel = nullptr);

    /// Igual que complete() pero pidiendo stream:true al servidor: entrega
    /// los incrementos de texto por on_delta conforme llegan y devuelve el
    /// resultado: en éxito, el finish_reason (CompletionInfo); el texto
    /// completo ya pasó por el callback. Reintenta igual que complete() mientras no se haya entregado
    /// ningún delta. Cancela si on_delta devuelve false o con cancel, igual
    /// que complete(). Nunca lanza excepciones.
    [[nodiscard]] Result<CompletionInfo> complete_stream(const std::vector<Message>& messages,
                                                         const StreamDeltaCallback& on_delta,
                                                         const CancelToken* cancel = nullptr);

    /// GET {base_url}/models: los id de modelo que da el servidor, sin el
    /// prefijo "models/" (Gemini), sin duplicados y en orden alfabético. No
    /// necesita modelo en la configuración (solo URL y key, o un servidor
    /// local). Mismo mapeo de errores, reintentos y cancelación que
    /// complete(); "data" ausente o que no es arreglo da BadResponse. Nunca
    /// lanza excepciones.
    [[nodiscard]] Result<std::vector<std::string>> list_models(const CancelToken* cancel = nullptr);

private:
    /// Envía request con la política de reintentos (sección 6) y devuelve el
    /// cuerpo de la respuesta 2xx, o el error mapeado.
    [[nodiscard]] Result<std::string> send_with_retries(const HttpRequest& request,
                                                        const CancelToken* cancel);
    Sleeper& sleeper();

    Config config_;
    std::unique_ptr<Transport> transport_;
    std::unique_ptr<Sleeper> sleeper_;
};

} // namespace chatbot

#endif // CHATBOT_CHAT_CLIENT_H
