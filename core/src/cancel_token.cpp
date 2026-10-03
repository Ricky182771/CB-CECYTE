#include "chatbot/cancel_token.h"

namespace chatbot {

void CancelToken::cancel() {
    {
        // Se marca con el mutex tomado para que quien está a punto de esperar
        // no pierda el aviso entre revisar la condición y dormirse.
        const std::lock_guard<std::mutex> lock(mutex_);
        cancelled_.store(true);
    }
    condition_.notify_all();
}

bool CancelToken::is_cancelled() const noexcept {
    return cancelled_.load();
}

bool CancelToken::wait_for(std::chrono::milliseconds duration) const {
    std::unique_lock<std::mutex> lock(mutex_);
    return condition_.wait_for(lock, duration, [this] { return cancelled_.load(); });
}

} // namespace chatbot
