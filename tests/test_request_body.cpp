#include "chatbot/chat_client.h"
#include "fake_transport.hpp"

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <string>

namespace {
using chatbot_test::make_client;
using chatbot_test::sample_messages;
using chatbot_test::test_config;
} // namespace

TEST_CASE("El cuerpo tiene model, stream false y los mensajes en orden", "[peticion]") {
    auto harness = make_client();
    harness.transport->responses.push_back(
        chatbot::HttpResponse{200, chatbot_test::success_body(), std::nullopt, false, ""});

    const chatbot::Result<std::string> response = harness.client->complete(sample_messages());
    REQUIRE(response.is_ok());
    REQUIRE(harness.transport->requests.size() == 1);

    const chatbot::HttpRequest& request = harness.transport->requests.front();
    const nlohmann::json body = nlohmann::json::parse(request.body);

    CHECK(body.at("model") == "modelo-de-prueba");
    CHECK(body.at("stream") == false);
    REQUIRE(body.at("messages").is_array());
    REQUIRE(body.at("messages").size() == 2);
    CHECK(body.at("messages")[0].at("role") == "system");
    CHECK(body.at("messages")[0].at("content") == "Eres un asistente de prueba.");
    CHECK(body.at("messages")[1].at("role") == "user");
    CHECK(body.at("messages")[1].at("content") == "Hola");
}

TEST_CASE("La URL es base_url + /chat/completions", "[peticion]") {
    auto harness = make_client();
    harness.transport->responses.push_back(
        chatbot::HttpResponse{200, chatbot_test::success_body(), std::nullopt, false, ""});

    (void)harness.client->complete(sample_messages());

    CHECK(harness.transport->requests.front().url ==
          "https://pruebas.invalid/v1/chat/completions");
}

TEST_CASE("La barra final de base_url se normaliza", "[peticion]") {
    chatbot::Config config = test_config();
    config.base_url = "https://pruebas.invalid/v1/";
    auto harness = make_client(config);
    harness.transport->responses.push_back(
        chatbot::HttpResponse{200, chatbot_test::success_body(), std::nullopt, false, ""});

    (void)harness.client->complete(sample_messages());

    CHECK(harness.transport->requests.front().url ==
          "https://pruebas.invalid/v1/chat/completions");
}

TEST_CASE("Los tres roles se serializan con su nombre correcto", "[peticion]") {
    auto harness = make_client();
    harness.transport->responses.push_back(
        chatbot::HttpResponse{200, chatbot_test::success_body(), std::nullopt, false, ""});

    const std::vector<chatbot::Message> messages{
        chatbot::Message{chatbot::Role::System, "s"},
        chatbot::Message{chatbot::Role::User, "u"},
        chatbot::Message{chatbot::Role::Assistant, "a"},
    };
    const chatbot::Result<std::string> response = harness.client->complete(messages);
    REQUIRE(response.is_ok());

    const nlohmann::json body = nlohmann::json::parse(harness.transport->requests.front().body);
    REQUIRE(body.at("messages").size() == 3);
    CHECK(body.at("messages")[0].at("role") == "system");
    CHECK(body.at("messages")[1].at("role") == "user");
    CHECK(body.at("messages")[2].at("role") == "assistant");
}

TEST_CASE("Historial vacío: error Config sin llamar al transporte", "[peticion]") {
    auto harness = make_client();
    const chatbot::Result<std::string> response = harness.client->complete({});

    REQUIRE(response.is_error());
    CHECK(response.error().kind == chatbot::ErrorKind::Config);
    CHECK(harness.transport->requests.empty());
}

TEST_CASE("Config sin key: error Config sin llamar al transporte", "[peticion]") {
    chatbot::Config config = test_config();
    config.api_key.clear();
    auto harness = make_client(config);

    const chatbot::Result<std::string> response = harness.client->complete(sample_messages());
    REQUIRE(response.is_error());
    CHECK(response.error().kind == chatbot::ErrorKind::Config);
    CHECK(harness.transport->requests.empty());
}

TEST_CASE("Config sin modelo: error Config sin llamar al transporte", "[peticion]") {
    chatbot::Config config = test_config();
    config.model.clear();
    auto harness = make_client(config);

    const chatbot::Result<std::string> response = harness.client->complete(sample_messages());
    REQUIRE(response.is_error());
    CHECK(response.error().kind == chatbot::ErrorKind::Config);
    CHECK(harness.transport->requests.empty());
}

TEST_CASE("El timeout configurado llega al transporte en milisegundos", "[peticion]") {
    chatbot::Config config = test_config();
    config.timeout_seconds = std::chrono::seconds{45};
    auto harness = make_client(config);
    harness.transport->responses.push_back(
        chatbot::HttpResponse{200, chatbot_test::success_body(), std::nullopt, false, ""});

    (void)harness.client->complete(sample_messages());

    CHECK(harness.transport->requests.front().timeout == std::chrono::milliseconds{45000});
}
