#ifndef CHATBOT_CHAT_CLIENT_H
#define CHATBOT_CHAT_CLIENT_H

#include "chatbot/config.h"
#include "chatbot/result.h"
#include "chatbot/sleeper.h"
#include "chatbot/transport.h"
#include "chatbot/types.h"

#include <functional>
#include <memory>
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
    [[nodiscard]] Result<std::string> complete(const std::vector<Message>& messages);

    /// Igual que complete() pero pidiendo stream:true al servidor: entrega
    /// los incrementos de texto por on_delta conforme llegan y devuelve solo
    /// el resultado (éxito o error); el texto completo ya pasó por el
    /// callback. Reintenta igual que complete() mientras no se haya entregado
    /// ningún delta. Nunca lanza excepciones.
    [[nodiscard]] Result<void> complete_stream(const std::vector<Message>& messages,
                                               const StreamDeltaCallback& on_delta);

private:
    Sleeper& sleeper();

    Config config_;
    std::unique_ptr<Transport> transport_;
    std::unique_ptr<Sleeper> sleeper_;
};

} // namespace chatbot

#endif // CHATBOT_CHAT_CLIENT_H
