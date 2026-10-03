#include "chatbot/history.h"

#include <iterator>

namespace chatbot {

TrimResult trim_history(const std::vector<Message>& messages, std::size_t limit_bytes) {
    std::size_t total = 0;
    for (const Message& message : messages) {
        total += message.content.size();
    }
    if (limit_bytes == 0 || total <= limit_bytes || messages.size() < 2) {
        return TrimResult{messages, 0};
    }

    // Bloque de sistema inicial: nunca se quita.
    std::size_t system_end = 0;
    while (system_end < messages.size() && messages[system_end].role == Role::System) {
        ++system_end;
    }

    // Los más viejos tras el sistema salen primero. Se quita el mensaje del
    // frente y luego todo lo que no sea User, para que el siguiente que
    // quede (si no es el último) empiece una vuelta con User.
    const std::size_t last = messages.size() - 1;
    std::size_t start = system_end;
    while (total > limit_bytes && start < last) {
        total -= messages[start].content.size();
        ++start;
        while (start < last && messages[start].role != Role::User) {
            total -= messages[start].content.size();
            ++start;
        }
    }

    TrimResult result;
    result.dropped = start - system_end;
    result.messages.reserve(messages.size() - result.dropped);
    result.messages.insert(result.messages.end(), messages.begin(),
                           std::next(messages.begin(), static_cast<std::ptrdiff_t>(system_end)));
    result.messages.insert(result.messages.end(),
                           std::next(messages.begin(), static_cast<std::ptrdiff_t>(start)),
                           messages.end());
    return result;
}

} // namespace chatbot
