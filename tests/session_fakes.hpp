// Lo falso para armar una chatbot::cli::Session sin red ni terminal: un
// directorio temporal (config.json, credentials.json, conversaciones), el
// entorno, el modelo (FakeTransport o un transporte propio) y Tavily (un
// TavilySearch con FakeTransport). Lo usan test_session.cpp y las pruebas de
// chatbot-rpc, así las dos prueban la misma Session.

#ifndef CHATBOT_TEST_SESSION_FAKES_HPP
#define CHATBOT_TEST_SESSION_FAKES_HPP

#include "session.h"

#include "blocking_transport.hpp"
#include "fake_search_provider.hpp"
#include "fake_transport.hpp"
#include "temp_dir.hpp"

#include "chatbot/chat_client.h"
#include "chatbot/config.h"
#include "chatbot/tavily_search.h"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <ctime>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace chatbot_test {

/// Fecha fija de /buscar y /exportar.
inline constexpr std::time_t kSessionNow = 1767225600; // 2026-01-01 UTC.
inline constexpr const char* kSearchKey = "tvly-clave-ficticia-de-busqueda";

/// Flujo SSE con un delta por pedazo (el texto ya escapado para JSON).
inline chatbot::HttpResponse stream(const std::string& piece) {
    chatbot::HttpResponse response;
    response.status = 200;
    response.body =
        "data: {\"choices\":[{\"delta\":{\"content\":\"" + piece + "\"}}]}\n\ndata: [DONE]\n\n";
    return response;
}

/// Como stream, con choices[0].finish_reason en el último evento.
inline chatbot::HttpResponse stream_with_finish(const std::string& piece,
                                                const std::string& reason) {
    chatbot::HttpResponse response;
    response.status = 200;
    response.body = "data: {\"choices\":[{\"delta\":{\"content\":\"" + piece +
                    "\"},\"finish_reason\":\"" + reason + "\"}]}\n\ndata: [DONE]\n\n";
    return response;
}

struct SessionFakes {
    ScopedTempDir dir;
    FakeEnv env;
    /// Respuestas del modelo de cada cliente nuevo (FakeTransport).
    std::vector<chatbot::HttpResponse> responses{stream("Hola, soy el asistente.")};
    /// Si está, el transporte de cada cliente nuevo en lugar del FakeTransport.
    std::function<std::unique_ptr<chatbot::Transport>()> chat_transport;
    FakeTransport* chat = nullptr; ///< El del último cliente.
    int clients = 0;
    std::vector<chatbot::HttpResponse> search_responses{tavily_response(
        nlohmann::json::array({{{"title", "T"}, {"url", "https://ejemplo.mx"}, {"content", "c"}}}))};
    /// Si está, el transporte de cada TavilySearch en lugar del FakeTransport.
    std::function<std::unique_ptr<chatbot::Transport>()> search_transport;
    std::optional<std::string> download_base;

    [[nodiscard]] std::string path(const char* name) const { return (dir.path() / name).string(); }
    [[nodiscard]] std::string config_path() const { return path("config.json"); }
    [[nodiscard]] std::string credentials_path() const { return path("credentials.json"); }

    /// Como main: load_config con allow_missing_key_and_model.
    [[nodiscard]] chatbot::Config load() const {
        chatbot::ConfigOptions options;
        options.env = env;
        options.path = [this] { return std::optional<std::string>{config_path()}; };
        options.credentials_path = [this] {
            return std::optional<std::string>{credentials_path()};
        };
        options.allow_missing_key_and_model = true;
        const chatbot::Result<chatbot::Config> config = chatbot::load_config(options);
        REQUIRE(config.is_ok());
        return config.value();
    }

    /// El SessionEnvironment con todo lo de arriba. Captura this: las
    /// SessionFakes deben vivir más que la Session.
    [[nodiscard]] chatbot::cli::SessionEnvironment environment() {
        chatbot::cli::SessionEnvironment environment;
        environment.config_path = config_path();
        environment.credentials_path = credentials_path();
        environment.data_dir = path("conversations");
        environment.env = [this](std::string_view name) { return env(name); };
        environment.make_client = [this](const chatbot::Config& complete) {
            ++clients;
            std::unique_ptr<chatbot::Transport> transport;
            if (chat_transport) {
                transport = chat_transport();
            } else {
                auto fake = std::make_unique<FakeTransport>();
                fake->responses = responses;
                chat = fake.get();
                transport = std::move(fake);
            }
            return std::make_unique<chatbot::ChatClient>(complete, std::move(transport),
                                                         std::make_unique<FakeSleeper>());
        };
        environment.make_search = [this](const std::string& key) {
            if (search_transport) {
                return std::make_shared<chatbot::TavilySearch>(key, search_transport());
            }
            auto fake = std::make_unique<FakeTransport>();
            fake->responses = search_responses;
            return std::make_shared<chatbot::TavilySearch>(key, std::move(fake));
        };
        environment.now = [] { return kSessionNow; };
        environment.download_base = [this] { return download_base; };
        return environment;
    }

    /// Cada cliente nuevo usa un BlockingTransport que se queda esperando
    /// después de un delta; blocking apunta al último.
    void block(BlockingTransport*& blocking) {
        chat_transport = [&blocking] {
            auto transport = std::make_unique<BlockingTransport>(BlockingTransport::Mode::Block);
            transport->first_chunk = "data: {\"choices\":[{\"delta\":{\"content\":\"parcial\"}}]}\n\n";
            blocking = transport.get();
            return transport;
        };
    }
};

} // namespace chatbot_test

#endif // CHATBOT_TEST_SESSION_FAKES_HPP
