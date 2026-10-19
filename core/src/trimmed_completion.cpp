#include "chatbot/trimmed_completion.h"

#include "chatbot/error.h"
#include "chatbot/history.h"

#include <algorithm>
#include <string_view>
#include <utility>

namespace chatbot {
namespace {

std::size_t total_bytes(const std::vector<Message>& messages) {
    std::size_t total = 0;
    for (const Message& message : messages) {
        total += message.content.size();
    }
    return total;
}

} // namespace

TrimmedCompletion complete_trimmed(ChatClient& client, const std::vector<Message>& history,
                                   std::size_t limit_bytes, const StreamDeltaCallback& on_delta,
                                   const CancelToken* cancel) {
    bool delivered = false;
    const StreamDeltaCallback deliver = [&](std::string_view delta) {
        delivered = true;
        return on_delta(delta);
    };

    const TrimResult first = trim_history(history, limit_bytes);
    TrimmedCompletion done{client.complete_stream(first.messages, deliver, cancel), first.dropped};

    // Reintento por contexto: una sola vez, si el servidor rechazó la
    // petición antes de mandar texto y recortar a la mitad quita algo más.
    if (done.result.is_error() && done.result.error().kind == ErrorKind::InvalidRequest &&
        !delivered && (cancel == nullptr || !cancel->is_cancelled())) {
        // Mitad de min(límite, tamaño real): si el historial ya es menor
        // que el límite, recortar a la mitad del límite no quitaría nada.
        const std::size_t current = total_bytes(first.messages);
        const std::size_t base = limit_bytes > 0 ? std::min(limit_bytes, current) : current;
        // trim_history interpreta 0 como "sin límite": el mínimo es 1.
        const TrimResult second = trim_history(history, std::max<std::size_t>(base / 2, 1));
        if (second.dropped > first.dropped) {
            done = TrimmedCompletion{client.complete_stream(second.messages, deliver, cancel),
                                     second.dropped};
        }
    }
    return done;
}

} // namespace chatbot
