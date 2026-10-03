#ifndef CHATBOT_COMPLETION_INFO_H
#define CHATBOT_COMPLETION_INFO_H

#include <string>

namespace chatbot {

/// Datos de una respuesta de streaming que terminó bien.
struct CompletionInfo {
    /// Último valor no nulo de choices[0].finish_reason que llegó ("stop",
    /// "length", "content_filter", ...), o vacío si nunca llegó.
    std::string finish_reason;
};

} // namespace chatbot

#endif // CHATBOT_COMPLETION_INFO_H
