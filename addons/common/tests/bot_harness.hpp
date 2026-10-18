#ifndef CHATBOT_BOT_TEST_HARNESS_HPP
#define CHATBOT_BOT_TEST_HARNESS_HPP

// Arnés de las pruebas de chatbot::bot: Outbox, reloj y registro falsos, y
// transportes que arman la respuesta a partir de la petición. Todo es
// seguro entre hilos: Bot los usa desde sus hilos de trabajo.

#include "blocking_transport.hpp"
#include "fake_transport.hpp"
#include "system_prompt_invariants.hpp"

#include "chatbot/bot/bot.h"
#include "chatbot/bot/config.h"
#include "chatbot/bot/outbox.h"
#include "chatbot/bot/types.h"

#include "chatbot/chat_client.h"
#include "chatbot/transport.h"
#include "chatbot/types.h"

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace chatbot_bot_test {

using chatbot::bot::ChatId;
using chatbot::bot::TextFormat;
using chatbot::bot::UserId;

/// Un mensaje que salió por el Outbox falso.
struct Sent {
    ChatId chat = 0;
    std::string text;
    TextFormat format = TextFormat::Plain;
};

class FakeOutbox final : public chatbot::bot::Outbox {
public:
    explicit FakeOutbox(std::size_t max_size = 4096) : max_size_(max_size) {}

    [[nodiscard]] chatbot::bot::MessageLimit message_limit() const override {
        return {max_size_, chatbot::bot::utf16_length};
    }

    bool send(ChatId chat, std::string_view text, TextFormat format) override {
        // Una plataforma lenta (límites de Telegram, red): la espera es
        // fuera del candado, como haría un cliente HTTP.
        if (const auto delay = send_delay.load(); delay.count() > 0) {
            std::this_thread::sleep_for(delay);
        }
        const std::lock_guard<std::mutex> lock(mutex_);
        if (format == TextFormat::Markdown && reject_markdown) {
            ++rejected_;
            return false;
        }
        sent_.push_back(Sent{chat, std::string{text}, format});
        changed_.notify_all();
        return true;
    }

    void set_typing(ChatId chat, bool typing) override {
        const std::lock_guard<std::mutex> lock(mutex_);
        typing_.emplace_back(chat, typing);
    }

    [[nodiscard]] std::vector<Sent> sent() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return sent_;
    }

    /// Lo que recibió un chat, en orden.
    [[nodiscard]] std::vector<std::string> texts_for(ChatId chat) const {
        const std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::string> out;
        for (const Sent& sent : sent_) {
            if (sent.chat == chat) {
                out.push_back(sent.text);
            }
        }
        return out;
    }

    [[nodiscard]] std::vector<std::pair<ChatId, bool>> typing() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return typing_;
    }

    [[nodiscard]] std::size_t rejected() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return rejected_;
    }

    /// Espera hasta que hayan salido al menos count mensajes.
    bool wait_for_count(std::size_t count,
                        std::chrono::milliseconds timeout = chatbot_test::kThreadTimeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        return changed_.wait_for(lock, timeout, [&] { return sent_.size() >= count; });
    }

    void clear() {
        const std::lock_guard<std::mutex> lock(mutex_);
        sent_.clear();
        typing_.clear();
    }

    /// true: los envíos en Markdown fallan (para probar el respaldo).
    std::atomic<bool> reject_markdown{false};
    /// Cuánto tarda cada envío.
    std::atomic<std::chrono::milliseconds> send_delay{std::chrono::milliseconds{0}};

private:
    std::size_t max_size_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<Sent> sent_;
    std::vector<std::pair<ChatId, bool>> typing_;
    std::size_t rejected_ = 0;
};

/// Reloj que solo avanza cuando la prueba lo pide. Empieza a mediodía local
/// para que avanzar unos minutos nunca cambie de día.
class FakeClock final : public chatbot::bot::Clock {
public:
    FakeClock() {
        std::tm noon{};
        noon.tm_year = 2026 - 1900;
        noon.tm_mon = 9; // Octubre.
        noon.tm_mday = 17;
        noon.tm_hour = 12;
        noon.tm_isdst = -1;
        now_ = std::chrono::system_clock::from_time_t(std::mktime(&noon));
    }

    [[nodiscard]] std::chrono::system_clock::time_point now() const override {
        const std::lock_guard<std::mutex> lock(mutex_);
        return now_;
    }

    void advance(std::chrono::system_clock::duration by) {
        const std::lock_guard<std::mutex> lock(mutex_);
        now_ += by;
    }

private:
    mutable std::mutex mutex_;
    std::chrono::system_clock::time_point now_;
};

class MemoryLog final : public chatbot::bot::Log {
public:
    void write(chatbot::bot::LogLevel /*level*/, std::string_view line) override {
        const std::lock_guard<std::mutex> lock(mutex_);
        lines_.emplace_back(line);
    }

    [[nodiscard]] std::vector<std::string> lines() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return lines_;
    }

    /// true si alguna línea contiene text.
    [[nodiscard]] bool contains(std::string_view text) const {
        const std::lock_guard<std::mutex> lock(mutex_);
        for (const std::string& line : lines_) {
            if (line.find(text) != std::string::npos) {
                return true;
            }
        }
        return false;
    }

private:
    mutable std::mutex mutex_;
    std::vector<std::string> lines_;
};

/// Flujo SSE con un delta por pedazo y [DONE]; finish_reason en el último
/// evento si no está vacío.
inline std::string sse_flow(const std::vector<std::string>& pieces,
                            const std::string& finish_reason = "stop") {
    std::string flow;
    for (const std::string& piece : pieces) {
        const nlohmann::json event = {
            {"choices", nlohmann::json::array({{{"delta", {{"content", piece}}}}})}};
        flow += "data: " + event.dump() + "\n\n";
    }
    if (!finish_reason.empty()) {
        const nlohmann::json event = {
            {"choices",
             nlohmann::json::array({{{"delta", nlohmann::json::object()},
                                     {"finish_reason", finish_reason}}})}};
        flow += "data: " + event.dump() + "\n\n";
    }
    return flow + "data: [DONE]\n\n";
}

/// Las peticiones que llegan al "modelo", de todos los clientes.
class Recorder {
public:
    void add(const chatbot::HttpRequest& request) {
        const std::lock_guard<std::mutex> lock(mutex_);
        requests_.push_back(request);
    }

    [[nodiscard]] std::vector<chatbot::HttpRequest> requests() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return requests_;
    }

    [[nodiscard]] std::size_t count() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return requests_.size();
    }

private:
    mutable std::mutex mutex_;
    std::vector<chatbot::HttpRequest> requests_;
};

/// Responde con lo que diga answer(mensajes enviados); por defecto "eco: "
/// y el último mensaje. Anota cada petición en el Recorder.
class ScriptedTransport final : public chatbot::Transport {
public:
    using Answer = std::function<chatbot::HttpResponse(const std::vector<chatbot::Message>&)>;

    ScriptedTransport(std::shared_ptr<Recorder> recorder, Answer answer)
        : recorder_(std::move(recorder)), answer_(std::move(answer)) {}

    [[nodiscard]] chatbot::HttpResponse send(const chatbot::HttpRequest& request) override {
        return send_stream(request, [](std::string_view, int) { return true; });
    }

    [[nodiscard]] chatbot::HttpResponse send_stream(
        const chatbot::HttpRequest& request, const chatbot::StreamCallback& on_chunk) override {
        recorder_->add(request);
        const chatbot::HttpResponse response =
            answer_(chatbot_test::sent_messages_of(request));
        if (response.status != 0 && !response.body.empty() &&
            !on_chunk(response.body, response.status)) {
            chatbot::HttpResponse cancelled;
            cancelled.cancelled = true;
            cancelled.error = "cancelado";
            return cancelled;
        }
        return response;
    }

private:
    std::shared_ptr<Recorder> recorder_;
    Answer answer_;
};

inline chatbot::HttpResponse ok_stream(const std::vector<std::string>& pieces,
                                       const std::string& finish_reason = "stop") {
    chatbot::HttpResponse response;
    response.status = 200;
    response.body = sse_flow(pieces, finish_reason);
    return response;
}

/// "eco: <último mensaje>".
inline chatbot::HttpResponse echo(const std::vector<chatbot::Message>& messages) {
    return ok_stream({"eco: ", messages.back().content});
}

/// Fábrica de clientes con ScriptedTransport y espera falsa (los reintentos
/// no esperan de verdad).
inline chatbot::bot::ClientFactory scripted_clients(std::shared_ptr<Recorder> recorder,
                                                    ScriptedTransport::Answer answer = echo) {
    return [recorder = std::move(recorder), answer = std::move(answer)] {
        return std::make_unique<chatbot::ChatClient>(
            chatbot_test::test_config(), std::make_unique<ScriptedTransport>(recorder, answer),
            std::make_unique<chatbot_test::FakeSleeper>());
    };
}

/// Compuerta compartida por los GateTransport: las peticiones esperan
/// adentro hasta que la prueba la abre (o se cancelan). Anota cuántas hay
/// adentro a la vez y en qué orden entraron.
class Gate {
public:
    /// Entra y espera a que se abra. false si se canceló antes.
    bool pass(const std::string& label, const chatbot::CancelToken* cancel) {
        std::unique_lock<std::mutex> lock(mutex_);
        ++inside_;
        max_inside_ = std::max(max_inside_, inside_);
        order_.push_back(label);
        changed_.notify_all();
        while (!open_) {
            if (cancel != nullptr && cancel->is_cancelled()) {
                --inside_;
                changed_.notify_all();
                return false;
            }
            changed_.wait_for(lock, std::chrono::milliseconds{10});
        }
        --inside_;
        changed_.notify_all();
        return true;
    }

    void open() {
        const std::lock_guard<std::mutex> lock(mutex_);
        open_ = true;
        changed_.notify_all();
    }

    /// Espera hasta que haya count peticiones adentro.
    bool wait_inside(std::size_t count,
                     std::chrono::milliseconds timeout = chatbot_test::kThreadTimeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        return changed_.wait_for(lock, timeout, [&] { return inside_ >= count; });
    }

    [[nodiscard]] std::size_t inside() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return inside_;
    }

    [[nodiscard]] std::size_t max_inside() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return max_inside_;
    }

    [[nodiscard]] std::vector<std::string> order() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return order_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    bool open_ = false;
    std::size_t inside_ = 0;
    std::size_t max_inside_ = 0;
    std::vector<std::string> order_;
};

/// Espera en la compuerta y después responde con eco.
class GateTransport final : public chatbot::Transport {
public:
    GateTransport(std::shared_ptr<Gate> gate, std::shared_ptr<Recorder> recorder)
        : gate_(std::move(gate)), recorder_(std::move(recorder)) {}

    [[nodiscard]] chatbot::HttpResponse send(const chatbot::HttpRequest& request) override {
        return send_stream(request, [](std::string_view, int) { return true; });
    }

    [[nodiscard]] chatbot::HttpResponse send_stream(
        const chatbot::HttpRequest& request, const chatbot::StreamCallback& on_chunk) override {
        recorder_->add(request);
        const std::vector<chatbot::Message> messages = chatbot_test::sent_messages_of(request);
        chatbot::HttpResponse cancelled;
        cancelled.cancelled = true;
        cancelled.error = "cancelado";
        if (!gate_->pass(messages.back().content, request.cancel)) {
            return cancelled;
        }
        const chatbot::HttpResponse response = echo(messages);
        return on_chunk(response.body, response.status) ? response : cancelled;
    }

private:
    std::shared_ptr<Gate> gate_;
    std::shared_ptr<Recorder> recorder_;
};

inline chatbot::bot::ClientFactory gated_clients(std::shared_ptr<Gate> gate,
                                                 std::shared_ptr<Recorder> recorder) {
    return [gate = std::move(gate), recorder = std::move(recorder)] {
        return std::make_unique<chatbot::ChatClient>(
            chatbot_test::test_config(), std::make_unique<GateTransport>(gate, recorder),
            std::make_unique<chatbot_test::FakeSleeper>());
    };
}

/// Streaming lento: manda la respuesta (eco) en pieces deltas, con una
/// pausa entre ellos, y se detiene si se cancela, como curl.
class SlowTransport final : public chatbot::Transport {
public:
    SlowTransport(std::size_t pieces, std::chrono::milliseconds pause)
        : pieces_(pieces), pause_(pause) {}

    [[nodiscard]] chatbot::HttpResponse send(const chatbot::HttpRequest& request) override {
        return send_stream(request, [](std::string_view, int) { return true; });
    }

    [[nodiscard]] chatbot::HttpResponse send_stream(
        const chatbot::HttpRequest& request, const chatbot::StreamCallback& on_chunk) override {
        const std::vector<chatbot::Message> messages = chatbot_test::sent_messages_of(request);
        std::vector<std::string> parts{"eco: "};
        for (std::size_t i = 0; i < pieces_; ++i) {
            parts.push_back(i == 0 ? messages.back().content : std::string{"."});
        }
        const std::string flow = sse_flow(parts);
        chatbot::HttpResponse cancelled;
        cancelled.cancelled = true;
        cancelled.error = "cancelado";
        // Un evento SSE por trozo.
        std::size_t pos = 0;
        while (pos < flow.size()) {
            const std::size_t end = flow.find("\n\n", pos) + 2;
            if ((request.cancel != nullptr && request.cancel->wait_for(pause_)) ||
                !on_chunk(std::string_view{flow}.substr(pos, end - pos), 200)) {
                return cancelled;
            }
            pos = end;
        }
        chatbot::HttpResponse response;
        response.status = 200;
        response.body = flow;
        return response;
    }

private:
    std::size_t pieces_;
    std::chrono::milliseconds pause_;
};

/// Configuración de prueba: usuario 1 autorizado, sin código.
inline chatbot::bot::BotConfig test_bot_config() {
    chatbot::bot::BotConfig config;
    config.allowed_users = {1};
    config.system_prompt = "Eres un bot de prueba.";
    return config;
}

inline chatbot::bot::ValidBotConfig valid(chatbot::bot::BotConfig config) {
    auto result = chatbot::bot::validate_bot_config(std::move(config));
    REQUIRE(result.is_ok());
    return result.value();
}

inline chatbot::bot::IncomingMessage message(ChatId chat, UserId user, std::string text) {
    return chatbot::bot::IncomingMessage{"prueba", chat, user, "Nombre", std::move(text)};
}

/// Todo lo de una prueba de Bot: salida, reloj, registro, peticiones y el
/// bot (se crea con start()).
struct BotHarness {
    FakeOutbox outbox;
    FakeClock clock;
    MemoryLog log;
    std::shared_ptr<Recorder> recorder = std::make_shared<Recorder>();
    std::unique_ptr<chatbot::bot::Bot> bot;

    explicit BotHarness(std::size_t max_size = 4096) : outbox(max_size) {}

    void start(chatbot::bot::BotConfig config, chatbot::bot::ClientFactory clients,
               chatbot::bot::SearchFactory search = {}) {
        bot = std::make_unique<chatbot::bot::Bot>(
            valid(std::move(config)),
            chatbot::bot::BotDependencies{std::move(clients), std::move(search), outbox, clock,
                                          log});
    }

    void start(chatbot::bot::BotConfig config = test_bot_config()) {
        start(std::move(config), scripted_clients(recorder));
    }

    /// Manda el mensaje y espera a que el bot termine lo que haya en la fila.
    void say(ChatId chat, UserId user, std::string text) {
        bot->handle(message(chat, user, std::move(text)));
        REQUIRE(bot->wait_idle(chatbot_test::kThreadTimeout));
    }
};

} // namespace chatbot_bot_test

#endif // CHATBOT_BOT_TEST_HARNESS_HPP
