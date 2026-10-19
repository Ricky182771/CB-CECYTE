#ifndef CHATBOT_TELEGRAM_TEST_HARNESS_HPP
#define CHATBOT_TELEGRAM_TEST_HARNESS_HPP

// Telegram falso para las pruebas del addon: un Transport que anota cada
// llamada a la Bot API (método, cuerpo JSON) y contesta lo que diga la
// prueba. Seguro entre hilos: lo usan el long polling, los hilos de trabajo
// del bot y el hilo del Outbox a la vez.

#include "blocking_transport.hpp"

#include "chatbot/telegram/api.h"

#include "chatbot/cancel_token.h"
#include "chatbot/transport.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace chatbot_telegram_test {

/// Token de prueba con la forma de uno real (no existe).
inline constexpr std::string_view kToken = "123456789:AAFakeTokenForTestsOnly_abcdefghijk";

struct Call {
    std::string method;
    std::string url;
    nlohmann::json body;
    std::chrono::milliseconds timeout{0};
};

inline chatbot::HttpResponse ok(const nlohmann::json& result = true) {
    chatbot::HttpResponse response;
    response.status = 200;
    response.body = nlohmann::json{{"ok", true}, {"result", result}}.dump();
    return response;
}

inline chatbot::HttpResponse failure(int status, std::string description,
                                     std::optional<int> retry_after = std::nullopt) {
    nlohmann::json body{{"ok", false}, {"error_code", status}, {"description", description}};
    if (retry_after.has_value()) {
        body["parameters"] = {{"retry_after", *retry_after}};
    }
    chatbot::HttpResponse response;
    response.status = status;
    response.body = body.dump();
    return response;
}

inline chatbot::HttpResponse no_network(std::string error = "Could not resolve host") {
    chatbot::HttpResponse response;
    response.error = std::move(error);
    return response;
}

/// Un update de mensaje de texto en un chat privado.
inline nlohmann::json text_update(std::int64_t id, std::int64_t chat, std::int64_t user,
                                  const std::string& text, std::int64_t date = 0,
                                  const std::string& type = "private") {
    nlohmann::json message{{"message_id", id},
                           {"date", date},
                           {"chat", {{"id", chat}, {"type", type}}},
                           {"from", {{"id", user}, {"is_bot", false}, {"first_name", "Ana"}}}};
    if (!text.empty()) {
        message["text"] = text;
    } else {
        message["photo"] = nlohmann::json::array({{{"file_id", "x"}}});
    }
    return {{"update_id", id}, {"message", message}};
}

inline nlohmann::json stop_update(std::int64_t id, std::int64_t chat, std::int64_t draft) {
    return {{"update_id", id},
            {"stopped_message_generation",
             {{"chat", {{"id", chat}, {"type", "private"}}}, {"draft_id", draft}}}};
}

/// El "servidor" de Telegram.
class FakeTelegram {
public:
    /// Lo que contesta cada método (set_respond); por defecto, ok.
    /// getUpdates contesta de la fila de push_updates y, si está vacía,
    /// espera (como el long polling) hasta que haya algo o se cancele.
    chatbot::HttpResponse handle(const chatbot::HttpRequest& request) {
        Call call;
        call.url = request.url;
        call.method = request.url.substr(request.url.rfind('/') + 1);
        call.timeout = request.timeout;
        try {
            call.body = nlohmann::json::parse(request.body);
        } catch (const nlohmann::json::exception&) {
            call.body = nullptr;
        }
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            calls_.push_back(call);
            changed_.notify_all();
        }
        if (call.method == "getUpdates") {
            return next_updates(request);
        }
        std::function<chatbot::HttpResponse(const Call&)> answer;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            answer = respond_;
        }
        return answer ? answer(call) : ok(nlohmann::json{{"message_id", 1}});
    }

    /// Agrega una respuesta de getUpdates.
    void push_updates(chatbot::HttpResponse response) {
        const std::lock_guard<std::mutex> lock(mutex_);
        updates_.push_back(std::move(response));
        changed_.notify_all();
    }

    void push_updates(const nlohmann::json& updates) { push_updates(ok(updates)); }

    void set_respond(std::function<chatbot::HttpResponse(const Call&)> answer) {
        const std::lock_guard<std::mutex> lock(mutex_);
        respond_ = std::move(answer);
    }

    [[nodiscard]] std::vector<Call> calls() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return calls_;
    }

    [[nodiscard]] std::vector<Call> calls(std::string_view method) const {
        const std::lock_guard<std::mutex> lock(mutex_);
        std::vector<Call> out;
        for (const Call& call : calls_) {
            if (call.method == method) {
                out.push_back(call);
            }
        }
        return out;
    }

    /// Espera hasta que haya count llamadas a method.
    bool wait_for(std::string_view method, std::size_t count,
                  std::chrono::milliseconds timeout = chatbot_test::kThreadTimeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        return changed_.wait_for(lock, timeout, [&] {
            std::size_t found = 0;
            for (const Call& call : calls_) {
                found += call.method == method ? 1 : 0;
            }
            return found >= count;
        });
    }

    void clear() {
        const std::lock_guard<std::mutex> lock(mutex_);
        calls_.clear();
    }

private:
    chatbot::HttpResponse next_updates(const chatbot::HttpRequest& request) {
        std::unique_lock<std::mutex> lock(mutex_);
        while (updates_.empty()) {
            if (request.cancel != nullptr && request.cancel->is_cancelled()) {
                chatbot::HttpResponse cancelled;
                cancelled.cancelled = true;
                cancelled.error = "cancelado";
                return cancelled;
            }
            // Como curl: revisa el token cada tanto.
            changed_.wait_for(lock, std::chrono::milliseconds{20});
        }
        chatbot::HttpResponse response = std::move(updates_.front());
        updates_.pop_front();
        return response;
    }

    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::function<chatbot::HttpResponse(const Call&)> respond_;
    std::vector<Call> calls_;
    std::deque<chatbot::HttpResponse> updates_;
};

class FakeTelegramTransport final : public chatbot::Transport {
public:
    explicit FakeTelegramTransport(std::shared_ptr<FakeTelegram> server)
        : server_(std::move(server)) {}

    [[nodiscard]] chatbot::HttpResponse send(const chatbot::HttpRequest& request) override {
        return server_->handle(request);
    }

    [[nodiscard]] chatbot::HttpResponse send_stream(const chatbot::HttpRequest& request,
                                                    const chatbot::StreamCallback&) override {
        return server_->handle(request);
    }

private:
    std::shared_ptr<FakeTelegram> server_;
};

inline chatbot::telegram::TransportFactory transports(std::shared_ptr<FakeTelegram> server) {
    return [server = std::move(server)] { return std::make_unique<FakeTelegramTransport>(server); };
}

} // namespace chatbot_telegram_test

#endif // CHATBOT_TELEGRAM_TEST_HARNESS_HPP
