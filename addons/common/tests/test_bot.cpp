#include "bot_harness.hpp"
#include "fake_search_provider.hpp"

#include "chatbot/bot/replies.h"

#include "chatbot/tavily_search.h"

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <atomic>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using chatbot::Message;
using chatbot::bot::TextFormat;
using chatbot_bot_test::BotHarness;
using chatbot_test::sent_messages_of;

chatbot::HttpResponse error_response(int status, const std::string& message) {
    chatbot::HttpResponse response;
    response.status = status;
    response.body = R"({"error":{"message":")" + message + R"("}})";
    return response;
}

} // namespace

TEST_CASE("bot: responde, con \"escribiendo…\" mientras prepara la respuesta", "[bot]") {
    BotHarness h;
    h.start();
    h.say(10, 1, "hola");
    const auto sent = h.outbox.sent();
    REQUIRE(sent.size() == 1);
    CHECK(sent[0].chat == 10);
    CHECK(sent[0].text == "eco: hola");
    CHECK(sent[0].format == TextFormat::Markdown);
    CHECK(h.outbox.typing() == std::vector<std::pair<chatbot::bot::ChatId, bool>>{{10, true},
                                                                                  {10, false}});
}

TEST_CASE("bot: una respuesta larga sale partida según el tope de la plataforma", "[bot]") {
    BotHarness h{120};
    std::string long_answer;
    for (int i = 0; i < 8; ++i) {
        long_answer += "Párrafo " + std::to_string(i) + " con texto suficiente para ocupar.\n\n";
    }
    h.start(chatbot_bot_test::test_bot_config(),
            chatbot_bot_test::scripted_clients(
                h.recorder, [long_answer](const std::vector<Message>&) {
                    return chatbot_bot_test::ok_stream({long_answer});
                }));
    h.say(10, 1, "cuéntame");
    const auto texts = h.outbox.texts_for(10);
    REQUIRE(texts.size() > 2);
    for (const std::string& text : texts) {
        CHECK(chatbot::bot::utf16_length(text) <= 120);
    }
    CHECK(h.log.contains("pedazos=" + std::to_string(texts.size())));
}

TEST_CASE("bot: si la plataforma rechaza el Markdown, manda texto plano", "[bot]") {
    BotHarness h;
    h.outbox.reject_markdown = true;
    h.start();
    h.say(10, 1, "hola");
    const auto sent = h.outbox.sent();
    REQUIRE(sent.size() == 1);
    CHECK(sent[0].text == "eco: hola");
    CHECK(sent[0].format == TextFormat::Plain);
    CHECK(h.outbox.rejected() == 1);
    CHECK(h.log.contains("enviado_como_texto chat=10"));
}

TEST_CASE("bot: un error del modelo llega amable y no entra al historial", "[bot]") {
    BotHarness h;
    std::atomic<bool> fail{true};
    h.start(chatbot_bot_test::test_bot_config(),
            chatbot_bot_test::scripted_clients(
                h.recorder, [&fail](const std::vector<Message>& messages) {
                    return fail ? error_response(401, "Invalid key clave-ficticia-de-prueba")
                                : chatbot_bot_test::echo(messages);
                }));
    h.say(10, 1, "uno");
    CHECK(h.outbox.texts_for(10) ==
          std::vector<std::string>{"El bot no está bien configurado. Avísale al administrador."});
    // El detalle va al registro, nunca al usuario.
    CHECK(h.log.contains("error chat=10 usuario=1 origen=modelo"));
    CHECK(h.log.contains("http=401"));
    fail = false;
    h.say(10, 1, "dos");
    CHECK(sent_messages_of(h.recorder->requests().back()).size() == 2); // Sin "uno".
}

TEST_CASE("bot: una respuesta vacía es un aviso y no entra al historial", "[bot]") {
    BotHarness h;
    h.start(chatbot_bot_test::test_bot_config(),
            chatbot_bot_test::scripted_clients(h.recorder, [](const std::vector<Message>& m) {
                return m.back().content == "uno" ? chatbot_bot_test::ok_stream({" \n "})
                                                 : chatbot_bot_test::echo(m);
            }));
    h.say(10, 1, "uno");
    CHECK(h.outbox.texts_for(10) == std::vector<std::string>{std::string{chatbot::bot::kEmptyAnswer}});
    h.say(10, 1, "dos");
    CHECK(sent_messages_of(h.recorder->requests().back()).size() == 2);
}

TEST_CASE("bot: la nota de finish_reason sale después de la respuesta", "[bot]") {
    BotHarness h;
    h.start(chatbot_bot_test::test_bot_config(),
            chatbot_bot_test::scripted_clients(h.recorder, [](const std::vector<Message>&) {
                return chatbot_bot_test::ok_stream({"Respuesta a medias"}, "length");
            }));
    h.say(10, 1, "hola");
    CHECK(h.outbox.texts_for(10) ==
          std::vector<std::string>{"Respuesta a medias", chatbot::bot::finish_note("length")});
}

TEST_CASE("bot: un mensaje que no es texto recibe un aviso", "[bot]") {
    BotHarness h;
    h.start();
    h.say(10, 1, "");
    CHECK(h.outbox.texts_for(10) == std::vector<std::string>{std::string{chatbot::bot::kTextOnly}});
    CHECK(h.recorder->count() == 0);
}

TEST_CASE("bot: el registro nunca lleva el texto del usuario ni el de la respuesta",
          "[bot][registro]") {
    const std::string user_text = "SECRETO-USUARIO-456";
    const std::string answer_text = "SECRETO-RESPUESTA-123";
    const std::string name = "NOMBRE-VISIBLE-789";
    BotHarness h;
    auto config = chatbot_bot_test::test_bot_config();
    config.invite_code = "Codigo_Secreto-1";
    h.start(config,
            chatbot_bot_test::scripted_clients(
                h.recorder,
                [answer_text](const std::vector<Message>& messages) {
                    if (messages.back().content.find("falla") != std::string::npos) {
                        return error_response(500, "sobrecarga");
                    }
                    return chatbot_bot_test::ok_stream({answer_text, " ", answer_text}, "length");
                }),
            [user_text] {
                // Resultados que repiten la consulta, como haría la web.
                auto transport = std::make_unique<chatbot_test::FakeTransport>();
                transport->responses.push_back(chatbot_test::tavily_response(
                    nlohmann::json::array({{{"title", user_text},
                                            {"url", "https://ejemplo.mx"},
                                            {"content", user_text}}})));
                return std::make_unique<chatbot::TavilySearch>("tvly-key-ficticia",
                                                               std::move(transport));
            });
    const auto say = [&](chatbot::bot::UserId user, const std::string& text) {
        h.bot->handle(chatbot::bot::IncomingMessage{"prueba", 10, user, name, text});
        REQUIRE(h.bot->wait_idle(chatbot_test::kThreadTimeout));
    };
    say(1, user_text);
    say(1, user_text + " falla");
    say(1, "/buscar " + user_text);
    say(1, "/nuevo");
    say(1, user_text);
    say(9, user_text);                         // No autorizado.
    say(9, "/start " + user_text);             // Código incorrecto.
    REQUIRE(h.outbox.texts_for(10).size() >= 6);
    REQUIRE_FALSE(h.log.lines().empty());
    for (const std::string& line : h.log.lines()) {
        INFO(line);
        CHECK(line.find("SECRETO") == std::string::npos);
        CHECK(line.find(name) == std::string::npos);
        CHECK(line.find("Codigo_Secreto") == std::string::npos);
    }
}

TEST_CASE("bot: después de apagarlo no atiende", "[bot]") {
    BotHarness h;
    h.start();
    h.bot->shutdown();
    h.bot->shutdown(); // Se puede llamar otra vez.
    h.bot->handle(chatbot_bot_test::message(10, 1, "hola"));
    CHECK(h.outbox.sent().empty());
    CHECK(h.recorder->count() == 0);
    CHECK(h.log.contains("apagado en_curso=0 descartadas=0"));
}
