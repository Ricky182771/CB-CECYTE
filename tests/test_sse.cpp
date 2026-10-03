#include "sse.h"

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>

namespace {

using chatbot::sse::decode_event;
using chatbot::sse::decode_openai_chunk;
using chatbot::sse::Event;
using chatbot::sse::EventType;
using chatbot::sse::Parser;

/// Chunk OpenAI con delta.content y su evento SSE ya armado.
std::string chunk_event(const std::string& text) {
    return "data: {\"choices\":[{\"delta\":{\"content\":\"" + text + "\"}}]}\n\n";
}

} // namespace

TEST_CASE("feed: un evento completo en un solo trozo", "[sse][parser]") {
    Parser parser;
    parser.feed("data: hola\n\n");
    const std::optional<std::string> event = parser.next_event();
    REQUIRE(event.has_value());
    CHECK(*event == "data: hola");
    CHECK_FALSE(parser.has_partial_data());
    CHECK_FALSE(parser.next_event().has_value());
}

TEST_CASE("feed: evento partido entre dos trozos", "[sse][parser]") {
    Parser parser;
    parser.feed("data: ho");
    CHECK_FALSE(parser.next_event().has_value());
    CHECK(parser.has_partial_data());
    parser.feed("la\n\n");
    const std::optional<std::string> event = parser.next_event();
    REQUIRE(event.has_value());
    CHECK(*event == "data: hola");
}

TEST_CASE("feed: separadores CRLF", "[sse][parser]") {
    Parser parser;
    parser.feed("data: hola\r\n\r\n");
    const std::optional<std::string> event = parser.next_event();
    REQUIRE(event.has_value());
    CHECK(*event == "data: hola");
}

TEST_CASE("feed: trozo con dos eventos los entrega en orden", "[sse][parser]") {
    Parser parser;
    parser.feed("data: uno\n\ndata: dos\n\n");
    const std::optional<std::string> first = parser.next_event();
    const std::optional<std::string> second = parser.next_event();
    REQUIRE(first.has_value());
    CHECK(*first == "data: uno");
    REQUIRE(second.has_value());
    CHECK(*second == "data: dos");
    CHECK_FALSE(parser.next_event().has_value());
}

TEST_CASE("feed: datos multilínea se unen con salto de línea", "[sse][parser]") {
    Parser parser;
    parser.feed("data: uno\ndata: dos\n\n");
    const std::optional<std::string> event = parser.next_event();
    REQUIRE(event.has_value());
    CHECK(*event == "data: uno\ndata: dos");
}

TEST_CASE("feed: sin línea en blanco no completa evento", "[sse][parser]") {
    Parser parser;
    parser.feed("data: incompleto");
    CHECK_FALSE(parser.next_event().has_value());
    CHECK(parser.has_partial_data());
}

TEST_CASE("decode_event: comentario puro se ignora", "[sse]") {
    const chatbot::Result<Event> event = decode_event(": keep-alive");
    REQUIRE(event.is_ok());
    CHECK(event.value().type == EventType::Comment);
}

TEST_CASE("decode_event: sentinela [DONE]", "[sse]") {
    const chatbot::Result<Event> event = decode_event("data: [DONE]");
    REQUIRE(event.is_ok());
    CHECK(event.value().type == EventType::Done);
}

TEST_CASE("decode_event: delta normal", "[sse]") {
    const chatbot::Result<Event> event = decode_event("data: {\"x\":1}");
    REQUIRE(event.is_ok());
    CHECK(event.value().type == EventType::Delta);
    CHECK(event.value().data == "{\"x\":1}");
}

TEST_CASE("decode_event: sin espacio tras data: también vale", "[sse]") {
    const chatbot::Result<Event> event = decode_event("data:{\"x\":1}");
    REQUIRE(event.is_ok());
    CHECK(event.value().type == EventType::Delta);
    CHECK(event.value().data == "{\"x\":1}");
}

TEST_CASE("decode_event: campos event/id/retry se ignoran sin fallar", "[sse]") {
    const chatbot::Result<Event> event = decode_event("event: add\ndata: {\"x\":1}\nid: 7");
    REQUIRE(event.is_ok());
    CHECK(event.value().type == EventType::Delta);
    CHECK(event.value().data == "{\"x\":1}");
}

TEST_CASE("decode_openai_chunk: extrae el delta de texto", "[sse]") {
    const chatbot::Result<std::optional<std::string>> delta = decode_openai_chunk(
        R"({"choices":[{"delta":{"content":"Hol"},"index":0}]})");
    REQUIRE(delta.is_ok());
    REQUIRE(delta.value().has_value());
    CHECK(*delta.value() == "Hol");
}

TEST_CASE("decode_openai_chunk: chunk de cierre sin content no aporta texto", "[sse]") {
    const chatbot::Result<std::optional<std::string>> delta = decode_openai_chunk(
        R"({"choices":[{"delta":{},"finish_reason":"stop"}]})");
    REQUIRE(delta.is_ok());
    CHECK_FALSE(delta.value().has_value());
}

TEST_CASE("decode_openai_chunk: delta no objeto es BadResponse", "[sse]") {
    const chatbot::Result<std::optional<std::string>> delta = decode_openai_chunk(
        R"({"choices":[{"delta":"texto"}]})");
    REQUIRE(delta.is_error());
    CHECK(delta.error().kind == chatbot::ErrorKind::BadResponse);
}

TEST_CASE("decode_openai_chunk: JSON malformado es BadResponse", "[sse]") {
    const chatbot::Result<std::optional<std::string>> delta = decode_openai_chunk("{oops");
    REQUIRE(delta.is_error());
    CHECK(delta.error().kind == chatbot::ErrorKind::BadResponse);
}

TEST_CASE("decode_openai_chunk: sin choices es BadResponse", "[sse]") {
    const chatbot::Result<std::optional<std::string>> delta = decode_openai_chunk(R"({"id":1})");
    REQUIRE(delta.is_error());
    CHECK(delta.error().kind == chatbot::ErrorKind::BadResponse);
}

TEST_CASE("decode_openai_chunk: choices vacío (chunk de usage) no aporta texto", "[sse][null]") {
    const chatbot::Result<std::optional<std::string>> delta = decode_openai_chunk(
        R"({"choices":[],"usage":{"prompt_tokens":5,"completion_tokens":2}})");
    REQUIRE(delta.is_ok());
    CHECK_FALSE(delta.value().has_value());
}

TEST_CASE("decode_openai_chunk: content null no aporta texto", "[sse][null]") {
    const chatbot::Result<std::optional<std::string>> delta = decode_openai_chunk(
        R"({"choices":[{"delta":{"role":"assistant","content":null}}]})");
    REQUIRE(delta.is_ok());
    CHECK_FALSE(delta.value().has_value());
}

TEST_CASE("decode_openai_chunk: error en el flujo devuelve el mensaje del servidor",
          "[sse][null]") {
    const chatbot::Result<std::optional<std::string>> delta = decode_openai_chunk(
        R"({"error":{"message":"Modelo sobrecargado, intenta de nuevo"}})");
    REQUIRE(delta.is_error());
    CHECK(delta.error().kind == chatbot::ErrorKind::BadResponse);
    CHECK(delta.error().message == "Modelo sobrecargado, intenta de nuevo");
}

TEST_CASE("decode_openai_chunk: error se revisa antes que choices y se trunca", "[sse][null]") {
    const std::string long_message(1000, 'x');
    const chatbot::Result<std::optional<std::string>> delta = decode_openai_chunk(
        R"({"choices":[{"delta":{"content":"no"}}],"error":{"message":")" + long_message +
        R"("}})");
    REQUIRE(delta.is_error());
    CHECK(delta.error().kind == chatbot::ErrorKind::BadResponse);
    CHECK(delta.error().message.size() < long_message.size());
    CHECK(delta.error().message.substr(0, 10) == "xxxxxxxxxx");
}

TEST_CASE("decode_openai_chunk: content que no es cadena es BadResponse", "[sse]") {
    const chatbot::Result<std::optional<std::string>> delta = decode_openai_chunk(
        R"({"choices":[{"delta":{"content":42}}]})");
    REQUIRE(delta.is_error());
    CHECK(delta.error().kind == chatbot::ErrorKind::BadResponse);
}

TEST_CASE("flujo completo: trozos arbitrarios producen los deltas en orden", "[sse][parser]") {
    const std::string flow =
        ": ping\n\n" + chunk_event("Ho") + chunk_event("la") +
        "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n" +
        "data: [DONE]\n\n";

    for (const std::size_t size : {std::size_t{1}, std::size_t{3}, std::size_t{7},
                                   std::size_t{64}}) {
        Parser parser;
        std::string joined;
        for (std::size_t begin = 0; begin < flow.size(); begin += size) {
            parser.feed(flow.substr(begin, size));
            while (const std::optional<std::string> raw = parser.next_event()) {
                const chatbot::Result<Event> event = decode_event(*raw);
                REQUIRE(event.is_ok());
                if (event.value().type != EventType::Delta) {
                    continue;
                }
                const chatbot::Result<std::optional<std::string>> delta =
                    decode_openai_chunk(event.value().data);
                REQUIRE(delta.is_ok());
                if (delta.value().has_value()) {
                    joined += *delta.value();
                }
            }
        }
        CHECK(joined == "Hola");
    }
}

namespace {
chatbot::ErrorKind stream_error_kind(const std::string& data) {
    const chatbot::Result<std::optional<std::string>> delta = decode_openai_chunk(data);
    REQUIRE(delta.is_error());
    return delta.error().kind;
}
} // namespace

TEST_CASE("decode_openai_chunk: error con código numérico se clasifica como estado HTTP",
          "[sse][error-flujo]") {
    CHECK(stream_error_kind(R"j({"error":{"message":"ResourceExhausted: Worker local total request limit reached (917/16)","type":"internal_server_error","code":500}})j") ==
          chatbot::ErrorKind::Server);
    CHECK(stream_error_kind(R"j({"error":{"message":"m","code":503}})j") == chatbot::ErrorKind::Server);
    CHECK(stream_error_kind(R"j({"error":{"message":"m","code":429}})j") ==
          chatbot::ErrorKind::RateLimited);
    CHECK(stream_error_kind(R"j({"error":{"message":"m","code":400}})j") ==
          chatbot::ErrorKind::InvalidRequest);
    CHECK(stream_error_kind(R"j({"error":{"message":"m","code":401}})j") == chatbot::ErrorKind::Auth);
}

TEST_CASE("decode_openai_chunk: código como cadena numérica", "[sse][error-flujo]") {
    CHECK(stream_error_kind(R"j({"error":{"message":"m","code":"429"}})j") ==
          chatbot::ErrorKind::RateLimited);
    CHECK(stream_error_kind(R"j({"error":{"message":"m","code":"502"}})j") ==
          chatbot::ErrorKind::Server);
}

TEST_CASE("decode_openai_chunk: sin código se clasifica por error.type", "[sse][error-flujo]") {
    CHECK(stream_error_kind(R"j({"error":{"message":"Service temporarily overloaded","type":"overloaded_error"}})j") ==
          chatbot::ErrorKind::Server);
    CHECK(stream_error_kind(R"j({"error":{"message":"m","type":"Service_Unavailable"}})j") ==
          chatbot::ErrorKind::Server);
    CHECK(stream_error_kind(R"j({"error":{"message":"m","type":"internal_server_error"}})j") ==
          chatbot::ErrorKind::Server);
    CHECK(stream_error_kind(R"j({"error":{"message":"m","type":"RATE_LIMIT"}})j") ==
          chatbot::ErrorKind::RateLimited);
    CHECK(stream_error_kind(R"j({"error":{"message":"m","type":"ResourceExhausted"}})j") ==
          chatbot::ErrorKind::RateLimited);
    // Código no numérico: se ignora y manda el tipo.
    CHECK(stream_error_kind(R"j({"error":{"message":"m","code":"abc","type":"overloaded"}})j") ==
          chatbot::ErrorKind::Server);
}

TEST_CASE("decode_openai_chunk: error sin código ni tipo reconocible es BadResponse",
          "[sse][error-flujo]") {
    CHECK(stream_error_kind(R"j({"error":{"message":"algo raro","type":"invalid_thing"}})j") ==
          chatbot::ErrorKind::BadResponse);
    CHECK(stream_error_kind(R"j({"error":{"message":"algo raro"}})j") ==
          chatbot::ErrorKind::BadResponse);
    const chatbot::Result<std::optional<std::string>> delta =
        decode_openai_chunk(R"j({"error":{"message":"Service temporarily overloaded","code":503}})j");
    REQUIRE(delta.is_error());
    CHECK(delta.error().message == "Service temporarily overloaded");
}
