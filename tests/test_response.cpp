#include "chatbot/chat_client.h"
#include "fake_transport.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

namespace {
using chatbot_test::make_client;
using chatbot_test::sample_messages;
} // namespace

TEST_CASE("Respuesta válida devuelve el content del asistente", "[respuesta]") {
    auto harness = make_client();
    harness.transport->responses.push_back(chatbot::HttpResponse{200, chatbot_test::success_body("Respuesta de prueba"), std::nullopt, false, ""});

    const chatbot::Result<std::string> response = harness.client->complete(sample_messages());
    REQUIRE(response.is_ok());
    CHECK(response.value() == "Respuesta de prueba");
}

TEST_CASE("Sin choices: BadResponse", "[respuesta]") {
    auto harness = make_client();
    harness.transport->responses.push_back(chatbot::HttpResponse{200, R"({"id": "x"})", std::nullopt, false, ""});

    const chatbot::Result<std::string> response = harness.client->complete(sample_messages());
    REQUIRE(response.is_error());
    CHECK(response.error().kind == chatbot::ErrorKind::BadResponse);
}

TEST_CASE("choices vacío: BadResponse", "[respuesta]") {
    auto harness = make_client();
    harness.transport->responses.push_back(chatbot::HttpResponse{200, R"({"choices": []})", std::nullopt, false, ""});

    const chatbot::Result<std::string> response = harness.client->complete(sample_messages());
    REQUIRE(response.is_error());
    CHECK(response.error().kind == chatbot::ErrorKind::BadResponse);
}

TEST_CASE("Sin content: BadResponse", "[respuesta]") {
    auto harness = make_client();
    harness.transport->responses.push_back(
        chatbot::HttpResponse{200, R"({"choices": [{"message": {"role": "assistant"}}]})", std::nullopt, false, ""});

    const chatbot::Result<std::string> response = harness.client->complete(sample_messages());
    REQUIRE(response.is_error());
    CHECK(response.error().kind == chatbot::ErrorKind::BadResponse);
}

TEST_CASE("JSON malformado: BadResponse", "[respuesta]") {
    auto harness = make_client();
    harness.transport->responses.push_back(chatbot::HttpResponse{200, "{oops", std::nullopt, false, ""});

    const chatbot::Result<std::string> response = harness.client->complete(sample_messages());
    REQUIRE(response.is_error());
    CHECK(response.error().kind == chatbot::ErrorKind::BadResponse);
}

TEST_CASE("Respuesta que no es objeto: BadResponse", "[respuesta]") {
    auto harness = make_client();
    harness.transport->responses.push_back(chatbot::HttpResponse{200, "[1,2,3]", std::nullopt, false, ""});

    const chatbot::Result<std::string> response = harness.client->complete(sample_messages());
    REQUIRE(response.is_error());
    CHECK(response.error().kind == chatbot::ErrorKind::BadResponse);
}

TEST_CASE("content que no es cadena: BadResponse", "[respuesta]") {
    auto harness = make_client();
    harness.transport->responses.push_back(chatbot::HttpResponse{200, R"({"choices": [{"message": {"content": 42}}]})", std::nullopt, false, ""});

    const chatbot::Result<std::string> response = harness.client->complete(sample_messages());
    REQUIRE(response.is_error());
    CHECK(response.error().kind == chatbot::ErrorKind::BadResponse);
}

TEST_CASE("content null: BadResponse con mensaje claro", "[respuesta][null]") {
    auto harness = make_client();
    harness.transport->responses.push_back(chatbot::HttpResponse{
        200, R"({"choices": [{"message": {"role": "assistant", "content": null}}]})",
        std::nullopt, false, ""});

    const chatbot::Result<std::string> response = harness.client->complete(sample_messages());
    REQUIRE(response.is_error());
    CHECK(response.error().kind == chatbot::ErrorKind::BadResponse);
    CHECK(response.error().message == "El modelo no devolvió texto en la respuesta.");
}

TEST_CASE("BadResponse no se reintenta", "[respuesta]") {
    auto harness = make_client();
    harness.transport->responses.push_back(chatbot::HttpResponse{200, "{oops", std::nullopt, false, ""});

    const chatbot::Result<std::string> response = harness.client->complete(sample_messages());
    REQUIRE(response.is_error());
    CHECK(harness.transport->requests.size() == 1);
    CHECK(harness.sleeper->sleeps.empty());
}
