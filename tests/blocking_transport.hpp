#ifndef CHATBOT_TEST_BLOCKING_TRANSPORT_HPP
#define CHATBOT_TEST_BLOCKING_TRANSPORT_HPP

#include "chatbot/cancel_token.h"
#include "chatbot/transport.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace chatbot_test {

/// Límite de toda espera entre hilos en las pruebas: si se excede, la prueba
/// falla en lugar de colgarse.
inline constexpr std::chrono::seconds kThreadTimeout{5};

/// Transporte de prueba para el hilo de trabajo. Cada petición avisa por
/// `started` y luego, según el modo:
/// - Block: manda `first_chunk` (si hay) y se queda esperando sin mandar nada
///   más, revisando request.cancel con wait_for en ciclo, como el callback
///   de progreso de curl. Al cancelarse devuelve cancelled == true.
/// - Respond: manda `first_chunk` (si hay) y devuelve `response` de inmediato.
/// Seguro entre hilos: el hilo de trabajo lo usa y la prueba lo consulta.
class BlockingTransport final : public chatbot::Transport {
public:
    enum class Mode { Block, Respond };

    explicit BlockingTransport(Mode mode, chatbot::HttpResponse response = {})
        : mode_(mode), response_(std::move(response)) {}

    /// Texto que se entrega con estado 200 antes de bloquear o responder.
    std::string first_chunk;

    /// Se marca al llegar cada petición (un CancelToken usado como aviso).
    chatbot::CancelToken started;

    [[nodiscard]] chatbot::HttpResponse send(const chatbot::HttpRequest& request) override {
        return send_stream(request, [](std::string_view, int) { return true; });
    }

    [[nodiscard]] chatbot::HttpResponse send_stream(
        const chatbot::HttpRequest& request,
        const chatbot::StreamCallback& on_chunk) override {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            requests_.push_back(request);
        }
        started.cancel();
        if (!first_chunk.empty() && !on_chunk(first_chunk, 200)) {
            return cancelled();
        }
        if (mode_ == Mode::Respond) {
            return response_;
        }
        // Block: espera la cancelación con un tope de seguridad, para que una
        // prueba rota no se cuelgue para siempre.
        const auto deadline = std::chrono::steady_clock::now() + 2 * kThreadTimeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (request.cancel != nullptr &&
                request.cancel->wait_for(std::chrono::milliseconds{50})) {
                return cancelled();
            }
            if (request.cancel == nullptr) {
                std::this_thread::sleep_for(std::chrono::milliseconds{50});
            }
        }
        chatbot::HttpResponse timed_out;
        timed_out.error = "tope de seguridad del transporte de prueba";
        return timed_out;
    }

    /// Copia de las peticiones recibidas hasta ahora.
    [[nodiscard]] std::vector<chatbot::HttpRequest> requests() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return requests_;
    }

private:
    static chatbot::HttpResponse cancelled() {
        chatbot::HttpResponse response;
        response.error = "cancelado";
        response.cancelled = true;
        response.retryable = false;
        return response;
    }

    Mode mode_;
    chatbot::HttpResponse response_;
    mutable std::mutex mutex_;
    std::vector<chatbot::HttpRequest> requests_;
};

/// Cola de tareas protegida: hace de "hilo de la interfaz" en las pruebas.
/// El hilo de trabajo mete tareas con post(); la prueba las ejecuta en su
/// propio hilo con run_until().
class TaskQueue {
public:
    void post(std::function<void()> task) {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            tasks_.push_back(std::move(task));
        }
        condition_.notify_all();
    }

    /// Ejecuta tareas en el hilo que llama hasta que done() sea true o se
    /// agote el tiempo. Devuelve done().
    bool run_until(const std::function<bool()>& done,
                   std::chrono::milliseconds timeout = kThreadTimeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!done()) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                if (!condition_.wait_until(lock, deadline, [this] { return !tasks_.empty(); })) {
                    return done();
                }
                task = std::move(tasks_.front());
                tasks_.pop_front();
            }
            task();
        }
        return true;
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<std::function<void()>> tasks_;
};

} // namespace chatbot_test

#endif // CHATBOT_TEST_BLOCKING_TRANSPORT_HPP
