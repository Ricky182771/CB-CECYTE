#ifndef CHATBOT_CANCEL_TOKEN_H
#define CHATBOT_CANCEL_TOKEN_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>

namespace chatbot {

/// Señal de cancelación compartida entre hilos: uno llama cancel() y otro
/// consulta is_cancelled() o espera con wait_for(). Quien lo crea es dueño de
/// su vida útil y debe mantenerlo vivo mientras dure la petición.
class CancelToken {
public:
    CancelToken() = default;
    CancelToken(const CancelToken&) = delete;
    CancelToken& operator=(const CancelToken&) = delete;
    CancelToken(CancelToken&&) = delete;
    CancelToken& operator=(CancelToken&&) = delete;

    /// Marca la cancelación y despierta a quien esté en wait_for().
    void cancel();

    /// Indica si ya se llamó cancel(). No bloquea ni lanza excepciones.
    [[nodiscard]] bool is_cancelled() const noexcept;

    /// Espera hasta que pase el tiempo o se llame cancel(). Devuelve true si
    /// se canceló (también si ya lo estaba al llamar).
    [[nodiscard]] bool wait_for(std::chrono::milliseconds duration) const;

private:
    std::atomic<bool> cancelled_{false};
    mutable std::mutex mutex_;
    mutable std::condition_variable condition_;
};

} // namespace chatbot

#endif // CHATBOT_CANCEL_TOKEN_H
