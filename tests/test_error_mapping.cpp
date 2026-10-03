#include "chatbot/chat_client.h"
#include "chatbot/error.h"
#include "fake_transport.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <string>
#include <string_view>

namespace {

using chatbot::ChatError;
using chatbot::ErrorKind;
using chatbot::HttpResponse;
using chatbot_test::make_client;
using chatbot_test::sample_messages;

} // namespace

TEST_CASE("2xx es éxito", "[mapeo]") {
    auto harness = make_client();
    harness.transport->responses.push_back(HttpResponse{200, chatbot_test::success_body(), std::nullopt, false, ""});

    const chatbot::Result<std::string> response = harness.client->complete(sample_messages());
    REQUIRE(response.is_ok());
    REQUIRE(response.value() == "Hola, soy el asistente.");
}

TEST_CASE("401 y 403 se mapean a Auth", "[mapeo]") {
    for (const int status : {401, 403}) {
        auto harness = make_client();
        harness.transport->responses.push_back(HttpResponse{status, "{}", std::nullopt, false, ""});

        const chatbot::Result<std::string> response = harness.client->complete(sample_messages());
        REQUIRE(response.is_error());
        const ChatError& error = response.error();
        CHECK(error.kind == ErrorKind::Auth);
        CHECK(error.http_status == status);
        // Auth no se reintenta.
        CHECK(harness.transport->requests.size() == 1);
        CHECK(harness.sleeper->sleeps.empty());
    }
}

TEST_CASE("404 y 410 se mapean a ModelNotFound", "[mapeo]") {
    for (const int status : {404, 410}) {
        auto harness = make_client();
        harness.transport->responses.push_back(HttpResponse{status, "{}", std::nullopt, false, ""});

        const chatbot::Result<std::string> response = harness.client->complete(sample_messages());
        REQUIRE(response.is_error());
        const ChatError& error = response.error();
        CHECK(error.kind == ErrorKind::ModelNotFound);
        CHECK(error.http_status == status);
        // ModelNotFound no se reintenta.
        CHECK(harness.transport->requests.size() == 1);
        CHECK(harness.sleeper->sleeps.empty());
    }
}

TEST_CASE("429 se mapea a RateLimited", "[mapeo]") {
    auto harness = make_client();
    harness.transport->responses.push_back(HttpResponse{429, "{}", std::nullopt, false, ""});

    const chatbot::Result<std::string> response = harness.client->complete(sample_messages());
    REQUIRE(response.is_error());
    CHECK(response.error().kind == ErrorKind::RateLimited);
    CHECK(response.error().http_status == 429);
}

TEST_CASE("5xx se mapea a Server", "[mapeo]") {
    for (const int status : {500, 502, 503, 599}) {
        auto harness = make_client();
        harness.transport->responses.push_back(HttpResponse{status, "{}", std::nullopt, false, ""});

        const chatbot::Result<std::string> response = harness.client->complete(sample_messages());
        REQUIRE(response.is_error());
        CHECK(response.error().kind == ErrorKind::Server);
        CHECK(response.error().http_status == status);
    }
}

TEST_CASE("Fallo de red (status 0 sin timeout) se mapea a Network", "[mapeo]") {
    auto harness = make_client();
    harness.transport->responses.push_back(HttpResponse{0, "", std::nullopt, false, "no se pudo resolver el servidor"});

    const chatbot::Result<std::string> response = harness.client->complete(sample_messages());
    REQUIRE(response.is_error());
    CHECK(response.error().kind == ErrorKind::Network);
    CHECK(response.error().http_status == 0);
}

TEST_CASE("Fallo de red con timeout se mapea a Timeout", "[mapeo]") {
    auto harness = make_client();
    harness.transport->responses.push_back(HttpResponse{0, "", std::nullopt, true, "tiempo de espera agotado"});

    const chatbot::Result<std::string> response = harness.client->complete(sample_messages());
    REQUIRE(response.is_error());
    CHECK(response.error().kind == ErrorKind::Timeout);
}

TEST_CASE("Cuerpo de error con error.message", "[mapeo]") {
    auto harness = make_client();
    harness.transport->responses.push_back(
        HttpResponse{401, R"({"error": {"message": "la API key no es válida"}})",
                     std::nullopt, false, ""});

    const chatbot::Result<std::string> response = harness.client->complete(sample_messages());
    REQUIRE(response.is_error());
    REQUIRE_THAT(response.error().message,
                 Catch::Matchers::ContainsSubstring("la API key no es válida"));
}

TEST_CASE("Cuerpo de error sin error.message usa el texto del estado", "[mapeo]") {
    auto harness = make_client();
    harness.transport->responses.push_back(HttpResponse{401, "{}", std::nullopt, false, ""});

    const chatbot::Result<std::string> response = harness.client->complete(sample_messages());
    REQUIRE(response.is_error());
    REQUIRE_THAT(response.error().message,
                 Catch::Matchers::ContainsSubstring("401"));
}

TEST_CASE("Cuerpo de error con detail como alternativa", "[mapeo]") {
    auto harness = make_client();
    harness.transport->responses.push_back(
        HttpResponse{429, R"({"detail": "demasiadas peticiones"})", std::nullopt, false, ""});

    const chatbot::Result<std::string> response = harness.client->complete(sample_messages());
    REQUIRE(response.is_error());
    REQUIRE(response.error().kind == ErrorKind::RateLimited);
    REQUIRE_THAT(response.error().message,
                 Catch::Matchers::ContainsSubstring("demasiadas peticiones"));
}

TEST_CASE("Los cuerpos de error largos se truncan", "[mapeo]") {
    auto harness = make_client();
    const std::string largo(1000, 'x');
    harness.transport->responses.push_back(
        HttpResponse{429, R"({"error": {"message": ")" + largo + R"("}})",
                     std::nullopt, false, ""});

    const chatbot::Result<std::string> response = harness.client->complete(sample_messages());
    REQUIRE(response.is_error());
    CHECK(response.error().message.size() < 400);
}

TEST_CASE("Retry-After se extrae del transport y queda en el error", "[mapeo]") {
    auto harness = make_client();
    harness.transport->responses.push_back(
        HttpResponse{429, "{}", std::string{"30"}, false, ""});

    const chatbot::Result<std::string> response = harness.client->complete(sample_messages());
    // Se agotan los reintentos y el error final conserva retry_after.
    REQUIRE(response.is_error());
    CHECK(response.error().kind == ErrorKind::RateLimited);
    REQUIRE(response.error().retry_after.has_value());
    CHECK(response.error().retry_after->count() == 30);
    CHECK(harness.sleeper->sleeps.size() == 3);
}

TEST_CASE("400, 413 y 422 se mapean a InvalidRequest sin reintento", "[mapeo][invalid]") {
    for (const int status : {400, 413, 422}) {
        INFO("estado HTTP " << status);
        auto harness = make_client();
        harness.transport->responses.push_back(HttpResponse{
            status, R"({"error":{"message":"maximum context length exceeded"}})",
            std::nullopt, false, ""});

        const chatbot::Result<std::string> response = harness.client->complete(sample_messages());
        REQUIRE(response.is_error());
        CHECK(response.error().kind == ErrorKind::InvalidRequest);
        CHECK(response.error().http_status == status);
        CHECK(response.error().message == "maximum context length exceeded");
        CHECK(harness.transport->requests.size() == 1);
        CHECK(harness.sleeper->sleeps.empty());
    }
}

TEST_CASE("InvalidRequest tiene etiqueta en español", "[mapeo][invalid]") {
    CHECK(std::string{chatbot::error_kind_label(ErrorKind::InvalidRequest)} ==
          "petición inválida");
}

TEST_CASE("InvalidRequest en streaming no se reintenta", "[mapeo][invalid]") {
    auto harness = make_client();
    harness.transport->responses.push_back(HttpResponse{400, "{}", std::nullopt, false, ""});

    const chatbot::Result<chatbot::CompletionInfo> response = harness.client->complete_stream(
        sample_messages(), [](std::string_view) { return true; });
    REQUIRE(response.is_error());
    CHECK(response.error().kind == ErrorKind::InvalidRequest);
    CHECK(harness.transport->requests.size() == 1);
}
