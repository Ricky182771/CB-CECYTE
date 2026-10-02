#ifndef CHATBOT_TEST_FAKE_TRANSPORT_HPP
#define CHATBOT_TEST_FAKE_TRANSPORT_HPP

#include "chatbot/chat_client.h"
#include "chatbot/config.h"
#include "chatbot/sleeper.h"
#include "chatbot/transport.h"
#include "chatbot/types.h"

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace chatbot_test {

/// Transporte falso: devuelve respuestas pregrabadas en orden y repite la
/// última si se acaban. Registra las peticiones recibidas (sección 12).
class FakeTransport final : public chatbot::Transport {
public:
    std::vector<chatbot::HttpResponse> responses;
    std::vector<chatbot::HttpRequest> requests;

    [[nodiscard]] chatbot::HttpResponse send(const chatbot::HttpRequest& request) override {
        requests.push_back(request);
        if (responses.empty()) {
            return chatbot::HttpResponse{};
        }
        const std::size_t index =
            next_index_ < responses.size() - 1 ? next_index_ : responses.size() - 1;
        ++next_index_;
        return responses.at(index);
    }

private:
    std::size_t next_index_ = 0;
};

/// Espera falsa: registra las duraciones pedidas sin dormir de verdad.
class FakeSleeper final : public chatbot::Sleeper {
public:
    std::vector<std::chrono::milliseconds> sleeps;

    void sleep_for(std::chrono::milliseconds duration) override {
        sleeps.push_back(duration);
    }
};

/// Entorno falso para load_config, sin tocar el entorno real del proceso.
class FakeEnv {
public:
    std::map<std::string, std::string, std::less<>> values;

    [[nodiscard]] std::string_view operator()(std::string_view name) const {
        const auto it = values.find(name);
        return it != values.end() ? std::string_view{it->second} : std::string_view{};
    }
};

/// Config válida de prueba con valores evidentemente ficticios.
inline chatbot::Config test_config() {
    chatbot::Config config;
    config.api_key = "clave-ficticia-de-prueba";
    config.base_url = "https://pruebas.invalid/v1";
    config.model = "modelo-de-prueba";
    config.timeout_seconds = std::chrono::seconds{30};
    return config;
}

/// Conversación de ejemplo: sistema + usuario, en ese orden.
inline std::vector<chatbot::Message> sample_messages() {
    return {
        chatbot::Message{chatbot::Role::System, "Eres un asistente de prueba."},
        chatbot::Message{chatbot::Role::User, "Hola"},
    };
}

/// Arnés: cliente listo con transporte y espera falsos, cuyos punteros
/// quedan accesibles para las aserciones.
struct TestHarness {
    std::unique_ptr<chatbot::ChatClient> client;
    FakeTransport* transport = nullptr;
    FakeSleeper* sleeper = nullptr;
};

inline TestHarness make_client(chatbot::Config config = test_config()) {
    auto transport = std::make_unique<FakeTransport>();
    auto sleeper = std::make_unique<FakeSleeper>();
    TestHarness harness;
    harness.transport = transport.get();
    harness.sleeper = sleeper.get();
    harness.client = std::make_unique<chatbot::ChatClient>(
        std::move(config), std::move(transport), std::move(sleeper));
    return harness;
}

/// Cuerpo de respuesta 2xx válido para el protocolo OpenAI.
inline std::string success_body(std::string content = "Hola, soy el asistente.") {
    return R"({"choices":[{"message":{"role":"assistant","content":")" + content +
            R"("}}]})";
}

} // namespace chatbot_test

#endif // CHATBOT_TEST_FAKE_TRANSPORT_HPP
