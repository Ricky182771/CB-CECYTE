#ifndef CHATBOT_TRIMMED_COMPLETION_H
#define CHATBOT_TRIMMED_COMPLETION_H

#include "chatbot/cancel_token.h"
#include "chatbot/chat_client.h"
#include "chatbot/completion_info.h"
#include "chatbot/result.h"
#include "chatbot/types.h"

#include <cstddef>
#include <vector>

namespace chatbot {

/// Resultado de complete_trimmed: el de complete_stream y cuántos mensajes
/// quitó el recorte del intento que se usó al final.
struct TrimmedCompletion {
    Result<CompletionInfo> result;
    std::size_t dropped = 0;
};

/// complete_stream con el historial recortado a limit_bytes (trim_history; 0
/// = sin límite). Si el servidor rechaza la petición con InvalidRequest
/// (p. ej. "maximum context length exceeded") antes de entregar texto y sin
/// cancelación, reintenta una sola vez recortando a la mitad de
/// min(limit_bytes, tamaño de lo enviado), si eso quita algún mensaje más.
/// La usan la interfaz (RequestRunner) y los addons. Nunca lanza excepciones.
[[nodiscard]] TrimmedCompletion complete_trimmed(ChatClient& client,
                                                 const std::vector<Message>& history,
                                                 std::size_t limit_bytes,
                                                 const StreamDeltaCallback& on_delta,
                                                 const CancelToken* cancel = nullptr);

} // namespace chatbot

#endif // CHATBOT_TRIMMED_COMPLETION_H
