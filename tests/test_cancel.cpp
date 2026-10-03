#include "chatbot/cancel_token.h"
#include "chatbot/chat_client.h"
#include "chatbot/sleeper.h"
#include "fake_transport.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <future>
#include <string>
#include <string_view>

namespace {

using chatbot_test::make_client;
using chatbot_test::sample_messages;
using chatbot_test::success_body;
using Clock = std::chrono::steady_clock;

/// Límite para toda espera entre hilos: si se excede, la prueba falla.
constexpr std::chrono::seconds kThreadTimeout{5};

chatbot::HttpResponse rate_limited() {
    return chatbot::HttpResponse{429, "{}", std::nullopt, false, ""};
}

chatbot::HttpResponse sse_ok() {
    return chatbot::HttpResponse{
        200, "data: {\"choices\":[{\"delta\":{\"content\":\"Hola\"}}]}\n\ndata: [DONE]\n\n",
        std::nullopt, false, ""};
}

bool accept_delta(std::string_view) { return true; }

} // namespace

TEST_CASE("CancelToken: empieza sin cancelar y cancel() lo marca", "[cancelacion]") {
    chatbot::CancelToken token;
    CHECK_FALSE(token.is_cancelled());
    token.cancel();
    CHECK(token.is_cancelled());
    CHECK(token.wait_for(std::chrono::milliseconds{0})); // Ya cancelado: regresa de inmediato.
}

TEST_CASE("CancelToken::wait_for devuelve false al agotar el tiempo", "[cancelacion]") {
    const chatbot::CancelToken token;
    const auto start = Clock::now();
    CHECK_FALSE(token.wait_for(std::chrono::milliseconds{30}));
    CHECK(Clock::now() - start >= std::chrono::milliseconds{30});
}

TEST_CASE("CancelToken::wait_for devuelve true rápido si otro hilo cancela", "[cancelacion][hilos]") {
    chatbot::CancelToken token;
    const auto start = Clock::now();
    std::future<bool> waiter = std::async(std::launch::async, [&token] {
        return token.wait_for(std::chrono::seconds{60});
    });
    token.cancel();
    REQUIRE(waiter.wait_for(kThreadTimeout) == std::future_status::ready);
    CHECK(waiter.get());
    CHECK(Clock::now() - start < std::chrono::seconds{2});
}

TEST_CASE("RealSleeper: sin cancelar completa la espera", "[cancelacion]") {
    chatbot::RealSleeper sleeper;
    const chatbot::CancelToken token;
    CHECK(sleeper.sleep_for(std::chrono::milliseconds{10}, &token));
    CHECK(sleeper.sleep_for(std::chrono::milliseconds{10}, nullptr));
}

TEST_CASE("RealSleeper: la cancelación desde otro hilo interrumpe la espera",
          "[cancelacion][hilos]") {
    chatbot::RealSleeper sleeper;
    chatbot::CancelToken token;
    const auto start = Clock::now();
    std::future<bool> waiter = std::async(std::launch::async, [&] {
        return sleeper.sleep_for(std::chrono::seconds{60}, &token);
    });
    token.cancel();
    REQUIRE(waiter.wait_for(kThreadTimeout) == std::future_status::ready);
    CHECK_FALSE(waiter.get());
    CHECK(Clock::now() - start < std::chrono::seconds{2});
}

TEST_CASE("complete: token cancelado antes de llamar no hace peticiones", "[cancelacion]") {
    auto harness = make_client();
    harness.transport->responses.push_back(
        chatbot::HttpResponse{200, success_body(), std::nullopt, false, ""});
    chatbot::CancelToken token;
    token.cancel();

    const chatbot::Result<std::string> response =
        harness.client->complete(sample_messages(), &token);
    REQUIRE(response.is_error());
    CHECK(response.error().kind == chatbot::ErrorKind::Cancelled);
    CHECK(harness.transport->requests.empty());
}

TEST_CASE("complete_stream: token cancelado antes de llamar no hace peticiones",
          "[cancelacion]") {
    auto harness = make_client();
    harness.transport->responses.push_back(sse_ok());
    chatbot::CancelToken token;
    token.cancel();

    const chatbot::Result<chatbot::CompletionInfo> response =
        harness.client->complete_stream(sample_messages(), accept_delta, &token);
    REQUIRE(response.is_error());
    CHECK(response.error().kind == chatbot::ErrorKind::Cancelled);
    CHECK(harness.transport->requests.empty());
}

TEST_CASE("complete: el token llega al transporte en HttpRequest::cancel", "[cancelacion]") {
    auto harness = make_client();
    harness.transport->responses.push_back(
        chatbot::HttpResponse{200, success_body(), std::nullopt, false, ""});
    const chatbot::CancelToken token;

    REQUIRE(harness.client->complete(sample_messages(), &token).is_ok());
    REQUIRE(harness.transport->requests.size() == 1);
    CHECK(harness.transport->requests[0].cancel == &token);
}

TEST_CASE("complete_stream: el token llega al transporte en HttpRequest::cancel",
          "[cancelacion]") {
    auto harness = make_client();
    harness.transport->responses.push_back(sse_ok());
    const chatbot::CancelToken token;

    REQUIRE(harness.client->complete_stream(sample_messages(), accept_delta, &token).is_ok());
    REQUIRE(harness.transport->requests.size() == 1);
    CHECK(harness.transport->requests[0].cancel == &token);
}

TEST_CASE("complete: cancelar durante la espera de reintento no hace más peticiones",
          "[cancelacion]") {
    auto harness = make_client();
    harness.transport->responses.push_back(rate_limited());
    harness.transport->responses.push_back(
        chatbot::HttpResponse{200, success_body(), std::nullopt, false, ""});
    chatbot::CancelToken token;
    harness.sleeper->on_sleep = [&token] { token.cancel(); };

    const chatbot::Result<std::string> response =
        harness.client->complete(sample_messages(), &token);
    REQUIRE(response.is_error());
    CHECK(response.error().kind == chatbot::ErrorKind::Cancelled);
    CHECK(harness.transport->requests.size() == 1);
    CHECK(harness.sleeper->sleeps.size() == 1);
}

TEST_CASE("complete_stream: cancelar durante la espera de reintento no hace más peticiones",
          "[cancelacion]") {
    auto harness = make_client();
    harness.transport->responses.push_back(rate_limited());
    harness.transport->responses.push_back(sse_ok());
    chatbot::CancelToken token;
    harness.sleeper->on_sleep = [&token] { token.cancel(); };

    const chatbot::Result<chatbot::CompletionInfo> response =
        harness.client->complete_stream(sample_messages(), accept_delta, &token);
    REQUIRE(response.is_error());
    CHECK(response.error().kind == chatbot::ErrorKind::Cancelled);
    CHECK(harness.transport->requests.size() == 1);
    CHECK(harness.sleeper->sleeps.size() == 1);
}

TEST_CASE("complete: respuesta cancelada del transporte no se reintenta", "[cancelacion]") {
    auto harness = make_client();
    harness.transport->responses.push_back(
        chatbot::HttpResponse{0, "", std::nullopt, false, "abortado", true});
    harness.transport->responses.push_back(
        chatbot::HttpResponse{200, success_body(), std::nullopt, false, ""});

    const chatbot::Result<std::string> response = harness.client->complete(sample_messages());
    REQUIRE(response.is_error());
    CHECK(response.error().kind == chatbot::ErrorKind::Cancelled);
    CHECK(harness.transport->requests.size() == 1);
    CHECK(harness.sleeper->sleeps.empty());
}

TEST_CASE("complete_stream: respuesta cancelada del transporte no se reintenta",
          "[cancelacion]") {
    auto harness = make_client();
    harness.transport->responses.push_back(
        chatbot::HttpResponse{0, "", std::nullopt, false, "abortado", true});
    harness.transport->responses.push_back(sse_ok());

    const chatbot::Result<chatbot::CompletionInfo> response =
        harness.client->complete_stream(sample_messages(), accept_delta);
    REQUIRE(response.is_error());
    CHECK(response.error().kind == chatbot::ErrorKind::Cancelled);
    CHECK(harness.transport->requests.size() == 1);
    CHECK(harness.sleeper->sleeps.empty());
}

TEST_CASE("complete: cancelar durante un intento exitoso devuelve Cancelled", "[cancelacion]") {
    auto harness = make_client();
    harness.transport->responses.push_back(
        chatbot::HttpResponse{200, success_body(), std::nullopt, false, ""});
    chatbot::CancelToken token;
    harness.transport->on_send = [&token] { token.cancel(); };

    const chatbot::Result<std::string> response =
        harness.client->complete(sample_messages(), &token);
    REQUIRE(response.is_error());
    CHECK(response.error().kind == chatbot::ErrorKind::Cancelled);
    CHECK(harness.transport->requests.size() == 1);
}

TEST_CASE("complete_stream: cancelar después de recibir todo devuelve Cancelled",
          "[cancelacion]") {
    auto harness = make_client();
    harness.transport->responses.push_back(sse_ok());
    chatbot::CancelToken token;

    // El usuario cancela justo cuando llegó el último texto.
    const chatbot::Result<chatbot::CompletionInfo> response = harness.client->complete_stream(
        sample_messages(),
        [&token](std::string_view) {
            token.cancel();
            return true;
        },
        &token);
    REQUIRE(response.is_error());
    CHECK(response.error().kind == chatbot::ErrorKind::Cancelled);
    CHECK(harness.transport->requests.size() == 1);
}
