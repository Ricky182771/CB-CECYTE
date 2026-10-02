#include "chatbot/chat_client.h"
#include "fake_transport.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>

namespace {
using chatbot_test::make_client;
using chatbot_test::sample_messages;
using chatbot_test::success_body;
} // namespace

TEST_CASE("RateLimited: 4 intentos totales con esperas 1s, 2s, 4s", "[reintentos]") {
    auto harness = make_client();
    for (int i = 0; i < 4; ++i) {
        harness.transport->responses.push_back(
            chatbot::HttpResponse{429, "{}", std::nullopt, false, ""});
    }

    const chatbot::Result<std::string> response = harness.client->complete(sample_messages());
    REQUIRE(response.is_error());
    CHECK(response.error().kind == chatbot::ErrorKind::RateLimited);
    CHECK(harness.transport->requests.size() == 4); // 1 original + 3 reintentos.
    REQUIRE(harness.sleeper->sleeps.size() == 3);
    CHECK(harness.sleeper->sleeps[0] == std::chrono::milliseconds{1000});
    CHECK(harness.sleeper->sleeps[1] == std::chrono::milliseconds{2000});
    CHECK(harness.sleeper->sleeps[2] == std::chrono::milliseconds{4000});
}

TEST_CASE("Server: también se reintenta hasta 4 intentos", "[reintentos]") {
    auto harness = make_client();
    for (int i = 0; i < 4; ++i) {
        harness.transport->responses.push_back(
            chatbot::HttpResponse{500, "{}", std::nullopt, false, ""});
    }

    const chatbot::Result<std::string> response = harness.client->complete(sample_messages());
    REQUIRE(response.is_error());
    CHECK(response.error().kind == chatbot::ErrorKind::Server);
    CHECK(harness.transport->requests.size() == 4);
    CHECK(harness.sleeper->sleeps.size() == 3);
}

TEST_CASE("Network: se reintenta", "[reintentos]") {
    auto harness = make_client();
    harness.transport->responses.push_back(
        chatbot::HttpResponse{0, "", std::nullopt, false, "no se pudo conectar"});
    harness.transport->responses.push_back(
        chatbot::HttpResponse{200, success_body(), std::nullopt, false, ""});

    const chatbot::Result<std::string> response = harness.client->complete(sample_messages());
    REQUIRE(response.is_ok());
    CHECK(harness.transport->requests.size() == 2);
    REQUIRE(harness.sleeper->sleeps.size() == 1);
    CHECK(harness.sleeper->sleeps[0] == std::chrono::milliseconds{1000});
}

TEST_CASE("Timeout: se reintenta", "[reintentos]") {
    auto harness = make_client();
    harness.transport->responses.push_back(
        chatbot::HttpResponse{0, "", std::nullopt, true, "tiempo agotado"});
    harness.transport->responses.push_back(
        chatbot::HttpResponse{200, success_body(), std::nullopt, false, ""});

    const chatbot::Result<std::string> response = harness.client->complete(sample_messages());
    REQUIRE(response.is_ok());
    CHECK(harness.transport->requests.size() == 2);
}

TEST_CASE("Recuperación tras 429: el reintento puede tener éxito", "[reintentos]") {
    auto harness = make_client();
    harness.transport->responses.push_back(
        chatbot::HttpResponse{429, "{}", std::nullopt, false, ""});
    harness.transport->responses.push_back(
        chatbot::HttpResponse{200, success_body(), std::nullopt, false, ""});

    const chatbot::Result<std::string> response = harness.client->complete(sample_messages());
    REQUIRE(response.is_ok());
    CHECK(harness.transport->requests.size() == 2);
}

TEST_CASE("Retry-After: se respeta", "[reintentos]") {
    auto harness = make_client();
    harness.transport->responses.push_back(
        chatbot::HttpResponse{429, "{}", std::string{"10"}, false, ""});
    harness.transport->responses.push_back(
        chatbot::HttpResponse{200, success_body(), std::nullopt, false, ""});

    const chatbot::Result<std::string> response = harness.client->complete(sample_messages());
    REQUIRE(response.is_ok());
    REQUIRE(harness.sleeper->sleeps.size() == 1);
    CHECK(harness.sleeper->sleeps[0] == std::chrono::milliseconds{10000});
}

TEST_CASE("Retry-After con valor absurdo se acota a 60 s", "[reintentos]") {
    auto harness = make_client();
    harness.transport->responses.push_back(
        chatbot::HttpResponse{429, "{}", std::string{"3600"}, false, ""});
    harness.transport->responses.push_back(
        chatbot::HttpResponse{200, success_body(), std::nullopt, false, ""});

    const chatbot::Result<std::string> response = harness.client->complete(sample_messages());
    REQUIRE(response.is_ok());
    REQUIRE(harness.sleeper->sleeps.size() == 1);
    CHECK(harness.sleeper->sleeps[0] == std::chrono::milliseconds{60000});
}

TEST_CASE("Retry-After no numérico: se usa el backoff exponencial", "[reintentos]") {
    auto harness = make_client();
    harness.transport->responses.push_back(
        chatbot::HttpResponse{429, "{}", std::string{"Mar, 01 oct 2026 12:00:00 GMT"}, false, ""});
    harness.transport->responses.push_back(
        chatbot::HttpResponse{200, success_body(), std::nullopt, false, ""});

    const chatbot::Result<std::string> response = harness.client->complete(sample_messages());
    REQUIRE(response.is_ok());
    REQUIRE(harness.sleeper->sleeps.size() == 1);
    CHECK(harness.sleeper->sleeps[0] == std::chrono::milliseconds{1000});
}

TEST_CASE("Auth y ModelNotFound: no se reintenta jamás", "[reintentos]") {
    for (const int status : {401, 403, 404, 410}) {
        auto harness = make_client();
        harness.transport->responses.push_back(
            chatbot::HttpResponse{status, "{}", std::nullopt, false, ""});

        const chatbot::Result<std::string> response = harness.client->complete(sample_messages());
        REQUIRE(response.is_error());
        CHECK(harness.transport->requests.size() == 1);
        CHECK(harness.sleeper->sleeps.empty());
    }
}

TEST_CASE("BadResponse no se reintenta", "[reintentos]") {
    auto harness = make_client();
    harness.transport->responses.push_back(
        chatbot::HttpResponse{200, "{oops", std::nullopt, false, ""});

    const chatbot::Result<std::string> response = harness.client->complete(sample_messages());
    REQUIRE(response.is_error());
    CHECK(harness.transport->requests.size() == 1);
    CHECK(harness.sleeper->sleeps.empty());
}
