#include "chatbot/chat_client.h"
#include "fake_transport.hpp"

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>
#include <string_view>
#include <vector>

namespace {
using chatbot_test::make_client;
using chatbot_test::sample_messages;
using chatbot_test::success_body;
} // namespace

/// Flujo SSE con delta "Hola" y cierre sin content (done implícito).
std::string sse_flow(const std::string& text = "Hola") {
    std::string flow;
    flow += "data: {\"choices\":[{\"delta\":{\"role\":\"assistant\",\"content\":\"" + text +
            "\"}}]}\n\n";
    flow += "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n";
    flow += "data: [DONE]\n\n";
    return flow;
}

TEST_CASE("complete_stream: pide stream:true al servidor", "[streaming]") {
    auto harness = make_client();
    harness.transport->responses.push_back(
        chatbot::HttpResponse{200, sse_flow(), std::nullopt, false, ""});

    const chatbot::Result<void> response =
        harness.client->complete_stream(sample_messages(), [](std::string_view) {
            return true;
        });

    REQUIRE(response.is_ok());
    const nlohmann::json body = nlohmann::json::parse(harness.transport->requests.front().body);
    CHECK(body.at("stream") == true);
}

TEST_CASE("complete_stream: entrega los deltas en orden con trozos partidos", "[streaming]") {
    auto harness = make_client();
    harness.transport->responses.push_back(
        chatbot::HttpResponse{200, sse_flow("uno-dos-tres"), std::nullopt, false, ""});
    harness.transport->chunk_size = 4;

    std::vector<std::string> deltas;
    const chatbot::Result<void> response = harness.client->complete_stream(
        sample_messages(), [&](std::string_view delta) {
            deltas.emplace_back(delta);
            return true;
        });

    REQUIRE(response.is_ok());
    CHECK_FALSE(deltas.empty());
    std::string joined;
    for (const std::string& delta : deltas) {
        joined += delta;
    }
    CHECK(joined == "uno-dos-tres");
}

TEST_CASE("complete_stream: flujo sin [DONE] al final se tolera", "[streaming]") {
    auto harness = make_client();
    harness.transport->responses.push_back(
        chatbot::HttpResponse{200, sse_flow(), std::nullopt, false, ""});
    // Corta el [DONE] final.
    chatbot::HttpResponse& response = harness.transport->responses.back();
    response.body.erase(response.body.size() - std::string{"data: [DONE]\n\n"}.size());

    std::string joined;
    const chatbot::Result<void> result = harness.client->complete_stream(
        sample_messages(), [&](std::string_view delta) {
            joined += delta;
            return true;
        });

    REQUIRE(result.is_ok());
    CHECK(joined == "Hola");
}

TEST_CASE("complete_stream: reintenta si falla antes del primer delta", "[streaming]") {
    auto harness = make_client();
    harness.transport->responses.push_back(
        chatbot::HttpResponse{429, "{}", std::nullopt, false, ""});
    harness.transport->responses.push_back(
        chatbot::HttpResponse{200, sse_flow(), std::nullopt, false, ""});

    std::vector<std::string> deltas;
    const chatbot::Result<void> response = harness.client->complete_stream(
        sample_messages(), [&](std::string_view delta) {
            deltas.emplace_back(delta);
            return true;
        });

    REQUIRE(response.is_ok());
    CHECK(harness.transport->requests.size() == 2);
    REQUIRE(harness.sleeper->sleeps.size() == 1);
    CHECK(harness.sleeper->sleeps[0] == std::chrono::milliseconds{1000});
    CHECK_FALSE(deltas.empty());
}

TEST_CASE("complete_stream: no reintenta si ya se entregó contenido", "[streaming]") {
    auto harness = make_client();
    // Primer intento: delta válido y después un evento con JSON roto a mitad
    // del flujo (antes del cierre).
    chatbot::HttpResponse broken;
    broken.status = 200;
    broken.body =
        "data: {\"choices\":[{\"delta\":{\"content\":\"parcial\"}}]}\n\n"
        "data: {\"no es sse\"\n\n"
        "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
        "data: [DONE]\n\n";
    harness.transport->responses.push_back(broken);

    std::vector<std::string> deltas;
    const chatbot::Result<void> response = harness.client->complete_stream(
        sample_messages(), [&](std::string_view delta) {
            deltas.emplace_back(delta);
            return true;
        });

    REQUIRE(response.is_error());
    CHECK(response.error().kind == chatbot::ErrorKind::BadResponse);
    CHECK(harness.transport->requests.size() == 1);
    CHECK(harness.sleeper->sleeps.empty());
    CHECK_FALSE(deltas.empty()); // El texto parcial ya se entregó.
}

TEST_CASE("complete_stream: cancelación desde el callback devuelve Cancelled", "[streaming]") {
    auto harness = make_client();
    harness.transport->responses.push_back(
        chatbot::HttpResponse{200, sse_flow("parcial"), std::nullopt, false, ""});

    const chatbot::Result<void> response = harness.client->complete_stream(
        sample_messages(), [](std::string_view) {
            return false; // Cancela al primer delta.
        });

    REQUIRE(response.is_error());
    CHECK(response.error().kind == chatbot::ErrorKind::Cancelled);
    CHECK(harness.transport->requests.size() == 1);
}

TEST_CASE("complete_stream: reintenta Server (500) antes del primer delta", "[streaming]") {
    auto harness = make_client();
    harness.transport->responses.push_back(
        chatbot::HttpResponse{500, "{}", std::nullopt, false, ""});
    harness.transport->responses.push_back(
        chatbot::HttpResponse{200, sse_flow(), std::nullopt, false, ""});

    std::vector<std::string> deltas;
    const chatbot::Result<void> response = harness.client->complete_stream(
        sample_messages(), [&](std::string_view delta) {
            deltas.emplace_back(delta);
            return true;
        });

    REQUIRE(response.is_ok());
    CHECK(harness.transport->requests.size() == 2);
    CHECK_FALSE(deltas.empty());
}

TEST_CASE("complete_stream: error Config no llama al transporte", "[streaming]") {
    chatbot::Config config = chatbot_test::test_config();
    config.model.clear();
    auto harness = make_client(config);

    const chatbot::Result<void> response = harness.client->complete_stream(
        sample_messages(), [](std::string_view) {
            return true;
        });

    REQUIRE(response.is_error());
    CHECK(response.error().kind == chatbot::ErrorKind::Config);
    CHECK(harness.transport->requests.empty());
}

TEST_CASE("complete_stream: BadResponse de un chunk no se reintenta", "[streaming]") {
    auto harness = make_client();
    chatbot::HttpResponse bad_chunk;
    bad_chunk.status = 200;
    bad_chunk.body = "data: {oops\n\n";
    harness.transport->responses.push_back(bad_chunk);

    const chatbot::Result<void> response = harness.client->complete_stream(
        sample_messages(), [](std::string_view) {
            return true;
        });

    REQUIRE(response.is_error());
    CHECK(response.error().kind == chatbot::ErrorKind::BadResponse);
    CHECK(harness.transport->requests.size() == 1);
    CHECK(harness.sleeper->sleeps.empty());
}
