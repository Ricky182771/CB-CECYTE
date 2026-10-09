#include "chatbot/chat_client.h"
#include "chatbot/config.h"
#include "fake_transport.hpp"
#include "temp_dir.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>

namespace {

using chatbot_test::make_client;
using chatbot_test::sample_messages;

/// Ruta única en un directorio temporal propio; se borra al salir del ámbito.
class TempPath {
public:
    explicit TempPath(const std::string& name) : path_{dir_.path() / name} {}

    [[nodiscard]] std::string string() const { return path_.string(); }
    [[nodiscard]] bool exists() const { return std::filesystem::exists(path_); }
    [[nodiscard]] std::string read() const {
        std::ifstream file{path_, std::ios::binary};
        return std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
    }

private:
    chatbot_test::ScopedTempDir dir_; ///< Antes que path_: se construye primero.
    std::filesystem::path path_;
};

std::size_t count(const std::string& text, std::string_view needle) {
    std::size_t total = 0;
    for (std::size_t at = text.find(needle); at != std::string::npos;
         at = text.find(needle, at + needle.size())) {
        ++total;
    }
    return total;
}

const std::string kSseBody =
    "data: {\"choices\":[{\"delta\":{\"content\":\"Hola\"}}]}\n\ndata: [DONE]\n\n";

bool accept(std::string_view) { return true; }

} // namespace

TEST_CASE("CHAT_DEBUG_SSE: sin la variable no se crea nada", "[depuracion]") {
    const TempPath dump{"sin_variable.txt"};
    auto harness = make_client(); // debug_sse_path vacío por defecto.
    harness.transport->responses.push_back(
        chatbot::HttpResponse{200, kSseBody, std::nullopt, false, ""});

    REQUIRE(harness.client->complete_stream(sample_messages(), accept).is_ok());
    CHECK_FALSE(dump.exists());
}

TEST_CASE("CHAT_DEBUG_SSE: un bloque por intento con el cuerpo crudo", "[depuracion]") {
    const TempPath dump{"intentos.txt"};
    chatbot::Config config = chatbot_test::test_config();
    config.debug_sse_path = dump.string();
    auto harness = make_client(config);
    harness.transport->responses.push_back(
        chatbot::HttpResponse{500, "{\"error\":{\"message\":\"caído\"}}", std::nullopt, false, ""});
    harness.transport->responses.push_back(
        chatbot::HttpResponse{200, kSseBody, std::nullopt, false, ""});

    REQUIRE(harness.client->complete_stream(sample_messages(), accept).is_ok());
    const std::string content = dump.read();
    CHECK(count(content, "intento: ") == 2);
    CHECK_THAT(content, Catch::Matchers::ContainsSubstring("intento: 1"));
    CHECK_THAT(content, Catch::Matchers::ContainsSubstring("intento: 2"));
    CHECK_THAT(content, Catch::Matchers::ContainsSubstring("estado HTTP: 500"));
    CHECK_THAT(content, Catch::Matchers::ContainsSubstring("estado HTTP: 200"));
    CHECK_THAT(content, Catch::Matchers::ContainsSubstring("modelo: modelo-de-prueba"));
    CHECK_THAT(content, Catch::Matchers::ContainsSubstring("{\"error\":{\"message\":\"caído\"}}"));
    CHECK_THAT(content, Catch::Matchers::ContainsSubstring(kSseBody));
}

TEST_CASE("CHAT_DEBUG_SSE: también registra complete sin streaming", "[depuracion]") {
    const TempPath dump{"complete.txt"};
    chatbot::Config config = chatbot_test::test_config();
    config.debug_sse_path = dump.string();
    auto harness = make_client(config);
    harness.transport->responses.push_back(
        chatbot::HttpResponse{200, chatbot_test::success_body(), std::nullopt, false, ""});

    REQUIRE(harness.client->complete(sample_messages()).is_ok());
    const std::string content = dump.read();
    CHECK(count(content, "intento: ") == 1);
    CHECK_THAT(content, Catch::Matchers::ContainsSubstring(chatbot_test::success_body()));
}

TEST_CASE("CHAT_DEBUG_SSE: la key y la petición nunca se escriben", "[depuracion]") {
    const TempPath dump{"sin_key.txt"};
    chatbot::Config config = chatbot_test::test_config();
    config.debug_sse_path = dump.string();
    auto harness = make_client(config);
    harness.transport->responses.push_back(
        chatbot::HttpResponse{200, kSseBody, std::nullopt, false, ""});

    REQUIRE(harness.client->complete_stream(sample_messages(), accept).is_ok());
    const std::string content = dump.read();
    REQUIRE_FALSE(content.empty());
    CHECK(content.find(config.api_key) == std::string::npos);
    CHECK(content.find("Authorization") == std::string::npos);
    CHECK(content.find("Eres un asistente de prueba.") == std::string::npos); // Cuerpo de la petición.
}

TEST_CASE("CHAT_DEBUG_SSE: una ruta inválida no produce error", "[depuracion]") {
    chatbot::Config config = chatbot_test::test_config();
    config.debug_sse_path = "/ruta/que/no/existe/chat-debug.txt";
    auto harness = make_client(config);
    harness.transport->responses.push_back(
        chatbot::HttpResponse{200, kSseBody, std::nullopt, false, ""});

    std::string joined;
    const chatbot::Result<chatbot::CompletionInfo> result = harness.client->complete_stream(
        sample_messages(), [&](std::string_view delta) {
            joined += delta;
            return true;
        });
    CHECK(result.is_ok());
    CHECK(joined == "Hola");
}

TEST_CASE("CHAT_DEBUG_SSE: load_config lo toma solo del entorno", "[depuracion][config]") {
    chatbot_test::FakeEnv env;
    env.values["CHAT_API_KEY"] = "clave-ficticia";
    env.values["CHAT_MODEL"] = "modelo-x";
    const auto no_file = [] { return std::optional<std::string>{}; };

    const chatbot::Result<chatbot::Config> without =
        chatbot::load_config(chatbot::ConfigOptions{env, no_file});
    REQUIRE(without.is_ok());
    CHECK_FALSE(without.value().debug_sse_path.has_value());

    env.values["CHAT_DEBUG_SSE"] = "/tmp/chat-debug.txt";
    const chatbot::Result<chatbot::Config> with =
        chatbot::load_config(chatbot::ConfigOptions{env, no_file});
    REQUIRE(with.is_ok());
    REQUIRE(with.value().debug_sse_path.has_value());
    CHECK(*with.value().debug_sse_path == "/tmp/chat-debug.txt");
}

TEST_CASE("CHAT_DEBUG_SSE: un error dentro del flujo queda con su cuerpo crudo",
          "[depuracion]") {
    const TempPath dump{"error_flujo.txt"};
    chatbot::Config config = chatbot_test::test_config();
    config.debug_sse_path = dump.string();
    auto harness = make_client(config);
    const std::string error_event =
        "data: {\"error\":{\"message\":\"ResourceExhausted\",\"type\":\"internal_server_error\","
        "\"code\":500}}\n\n";
    harness.transport->responses.push_back(
        chatbot::HttpResponse{200, error_event, std::nullopt, false, ""});
    harness.transport->responses.push_back(
        chatbot::HttpResponse{200, kSseBody, std::nullopt, false, ""});

    REQUIRE(harness.client->complete_stream(sample_messages(), accept).is_ok());
    const std::string content = dump.read();
    CHECK(count(content, "intento: ") == 2);
    CHECK(count(content, "estado HTTP: 200") == 2);
    CHECK_THAT(content, Catch::Matchers::ContainsSubstring(error_event));
}
