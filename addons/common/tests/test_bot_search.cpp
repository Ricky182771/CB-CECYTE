#include "bot_harness.hpp"
#include "fake_search_provider.hpp"

#include "chatbot/bot/replies.h"

#include "chatbot/search_context.h"
#include "chatbot/tavily_search.h"

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <memory>
#include <string>
#include <vector>

namespace {

using chatbot::Message;
using chatbot::Role;
using chatbot_bot_test::BotHarness;
using chatbot_bot_test::Recorder;
using chatbot_test::sent_messages_of;

/// Responde siempre lo mismo y anota cada petición (la búsqueda no manda
/// mensajes de chat, así que no sirve ScriptedTransport).
class CannedTransport final : public chatbot::Transport {
public:
    CannedTransport(chatbot::HttpResponse response, std::shared_ptr<Recorder> recorder)
        : response_(std::move(response)), recorder_(std::move(recorder)) {}

    [[nodiscard]] chatbot::HttpResponse send(const chatbot::HttpRequest& request) override {
        recorder_->add(request);
        return response_;
    }

    [[nodiscard]] chatbot::HttpResponse send_stream(const chatbot::HttpRequest& request,
                                                    const chatbot::StreamCallback&) override {
        return send(request);
    }

private:
    chatbot::HttpResponse response_;
    std::shared_ptr<Recorder> recorder_;
};

chatbot::bot::SearchFactory tavily(chatbot::HttpResponse response,
                                   std::shared_ptr<Recorder> searches) {
    return [response = std::move(response), searches = std::move(searches)] {
        return std::make_unique<chatbot::TavilySearch>(
            "tvly-key-ficticia-de-prueba", std::make_unique<CannedTransport>(response, searches));
    };
}

const nlohmann::json kResults = nlohmann::json::array(
    {{{"title", "Clima en Puebla"},
      {"url", "https://clima.example/puebla"},
      {"content", "Soleado, 24 grados."},
      {"published_date", "Tue, 11 Mar 2025 17:00:00 GMT"}},
     {{"title", "Pronóstico"}, {"url", "https://pronostico.example"}, {"content", "Sin lluvia."}}});

} // namespace

TEST_CASE("buscar: sin key no hay búsqueda ni llamada al modelo", "[bot][buscar]") {
    BotHarness h;
    auto config = chatbot_bot_test::test_bot_config();
    config.daily_message_limit = 1;
    h.start(config); // Sin SearchFactory.
    h.say(10, 1, "/buscar clima");
    h.say(10, 1, "/buscar");
    CHECK(h.outbox.texts_for(10) ==
          std::vector<std::string>(2, std::string{chatbot::bot::kSearchUnavailable}));
    CHECK(h.recorder->count() == 0);
    // No gastó el mensaje del día.
    h.say(10, 1, "hola");
    CHECK(h.outbox.texts_for(10).back() == "eco: hola");
    // La ayuda no ofrece /buscar.
    h.say(10, 1, "/ayuda");
    CHECK(h.outbox.texts_for(10).back().find("/buscar") == std::string::npos);
}

TEST_CASE("buscar: con key, los resultados van como datos y las fuentes después",
          "[bot][buscar]") {
    BotHarness h;
    const auto searches = std::make_shared<Recorder>();
    h.start(chatbot_bot_test::test_bot_config(), chatbot_bot_test::scripted_clients(h.recorder),
            tavily(chatbot_test::tavily_response(kResults), searches));
    h.say(10, 1, "/buscar clima en Puebla");

    // A Tavily solo va la consulta.
    REQUIRE(searches->count() == 1);
    const nlohmann::json search_body = nlohmann::json::parse(searches->requests()[0].body);
    CHECK(search_body.at("query") == "clima en Puebla");

    // Al modelo, el bloque con nonce en lugar del texto del comando: un solo
    // User, nunca en un System.
    REQUIRE(h.recorder->count() == 1);
    const std::vector<Message> sent = sent_messages_of(h.recorder->requests()[0]);
    REQUIRE(sent.size() == 2);
    CHECK(sent[0].role == Role::System);
    CHECK(sent[0].content == "Eres un bot de prueba.");
    CHECK(sent[1].role == Role::User);
    const std::string& block = sent[1].content;
    CHECK(block.find("<resultados id=\"") != std::string::npos);
    CHECK(block.find("Soleado, 24 grados.") != std::string::npos);
    CHECK(block.find("clima en Puebla") != std::string::npos);
    CHECK(block.find("/buscar") == std::string::npos);

    // Respuesta y fuentes (texto plano, de la búsqueda).
    const auto sent_out = h.outbox.sent();
    REQUIRE(sent_out.size() == 2);
    CHECK(sent_out[0].format == chatbot::bot::TextFormat::Markdown);
    CHECK(sent_out[0].text.rfind("eco: ", 0) == 0);
    CHECK(sent_out[1].format == chatbot::bot::TextFormat::Plain);
    CHECK(sent_out[1].text == "Fuentes:\n"
                              "[1] Clima en Puebla — 2025-03-11\nhttps://clima.example/puebla\n"
                              "[2] Pronóstico\nhttps://pronostico.example");

    // La siguiente pregunta lleva el bloque en el historial, como en la app.
    h.say(10, 1, "¿y mañana?");
    const std::vector<Message> next = sent_messages_of(h.recorder->requests()[1]);
    REQUIRE(next.size() == 4);
    CHECK(next[1].content == block);
    CHECK(next[3].content == "¿y mañana?");
}

TEST_CASE("buscar: cero resultados no llama al modelo", "[bot][buscar]") {
    BotHarness h;
    const auto searches = std::make_shared<Recorder>();
    h.start(chatbot_bot_test::test_bot_config(), chatbot_bot_test::scripted_clients(h.recorder),
            tavily(chatbot_test::tavily_response(nlohmann::json::array()), searches));
    h.say(10, 1, "/buscar nada de nada");
    CHECK(searches->count() == 1);
    CHECK(h.recorder->count() == 0);
    CHECK(h.outbox.texts_for(10) == std::vector<std::string>{std::string{chatbot::kNoSearchResults}});
    // El historial no guardó nada.
    h.say(10, 1, "hola");
    CHECK(sent_messages_of(h.recorder->requests().back()).size() == 2);
}

TEST_CASE("buscar: un error de la búsqueda llega como error de la búsqueda", "[bot][buscar]") {
    BotHarness h;
    const auto searches = std::make_shared<Recorder>();
    chatbot::HttpResponse unauthorized;
    unauthorized.status = 401;
    unauthorized.body = R"({"detail":{"error":"Unauthorized: missing or invalid API key."}})";
    h.start(chatbot_bot_test::test_bot_config(), chatbot_bot_test::scripted_clients(h.recorder),
            tavily(unauthorized, searches));
    h.say(10, 1, "/buscar algo");
    CHECK(h.recorder->count() == 0);
    CHECK(h.outbox.texts_for(10) ==
          std::vector<std::string>{
              "La búsqueda web no está bien configurada. Avísale al administrador."});
    CHECK(h.log.contains("origen=busqueda"));
}

TEST_CASE("buscar: sin consulta, el aviso de uso", "[bot][buscar]") {
    BotHarness h;
    const auto searches = std::make_shared<Recorder>();
    h.start(chatbot_bot_test::test_bot_config(), chatbot_bot_test::scripted_clients(h.recorder),
            tavily(chatbot_test::tavily_response(kResults), searches));
    h.say(10, 1, "/buscar   ");
    CHECK(h.outbox.texts_for(10) == std::vector<std::string>{std::string{chatbot::bot::kSearchUsage}});
    CHECK(searches->count() == 0);
    h.say(10, 1, "/ayuda");
    CHECK(h.outbox.texts_for(10).back().find("/buscar") != std::string::npos);
}

TEST_CASE("buscar: sin resultados o con error no gasta el tope diario", "[bot][buscar]") {
    BotHarness h;
    auto config = chatbot_bot_test::test_bot_config();
    config.daily_message_limit = 1;
    const auto searches = std::make_shared<Recorder>();
    h.start(config, chatbot_bot_test::scripted_clients(h.recorder),
            tavily(chatbot_test::tavily_response(nlohmann::json::array()), searches));
    h.say(10, 1, "/buscar nada");
    h.say(10, 1, "/buscar tampoco");
    CHECK(h.recorder->count() == 0);
    // El único mensaje del día sigue disponible para el modelo.
    h.say(10, 1, "hola");
    CHECK(h.outbox.texts_for(10).back() == "eco: hola");
    h.say(10, 1, "otra");
    CHECK(h.outbox.texts_for(10).back() == chatbot::bot::daily_limit_reached(1));
}
