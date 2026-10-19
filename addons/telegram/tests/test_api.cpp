// Cliente de la Bot API (BotApi) con un Telegram falso: peticiones, updates,
// errores y que el token nunca salga en un mensaje.

#include "telegram_harness.hpp"

#include "chatbot/telegram/api.h"
#include "chatbot/telegram/startup.h"

#include "bot_harness.hpp"
#include "fake_transport.hpp"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>

namespace {

using chatbot::ErrorKind;
using chatbot::telegram::BotApi;
using chatbot_telegram_test::Call;
using chatbot_telegram_test::FakeTelegram;
using chatbot_telegram_test::kToken;
using nlohmann::json;

struct ApiHarness {
    std::shared_ptr<FakeTelegram> server = std::make_shared<FakeTelegram>();
    BotApi api{std::string{kToken}, chatbot_telegram_test::transports(server)};
};

bool has_token(std::string_view text) {
    const std::string token{kToken};
    const std::string secret = token.substr(token.find(':') + 1);
    return text.find(token) != std::string_view::npos ||
           text.find(secret) != std::string_view::npos;
}

} // namespace

TEST_CASE("api: el token tiene que tener la forma de uno de @BotFather", "[telegram][api]") {
    CHECK(chatbot::telegram::plausible_token(kToken));
    for (const char* bad : {"", "123456", ":AAAAAAAAAAAAAAAAAAAAAAAA", "abc:AAAAAAAAAAAAAAAAAAAAAAAA",
                            "123:corto", "123:AAAAAAAAAAAAAAAAAAAAAAAA/getMe?x=",
                            "123:AAAAAAAAAAAAAAAAAAAA AAAA", "123:AAAAAAAAAAAAAAAAAAAAAAA#"}) {
        INFO(bad);
        CHECK_FALSE(chatbot::telegram::plausible_token(bad));
    }
}

TEST_CASE("api: getUpdates con varios updates, offset y allowed_updates", "[telegram][api]") {
    ApiHarness h;
    h.server->push_updates(json::array(
        {chatbot_telegram_test::text_update(10, 5, 5, "hola", 1700000000),
         chatbot_telegram_test::text_update(11, 5, 5, ""),
         chatbot_telegram_test::stop_update(12, 5, 7),
         json{{"update_id", 13}, {"edited_message", {{"text", "x"}}}},
         json{{"sin_id", true}}}));
    const auto result = h.api.get_updates(0, std::chrono::seconds{50});
    REQUIRE(result.is_ok());
    const auto& updates = result.value();
    REQUIRE(updates.size() == 4);
    CHECK(updates[0].update_id == 10);
    REQUIRE(updates[0].message.has_value());
    CHECK(updates[0].message->chat_id == 5);
    CHECK(updates[0].message->chat_type == "private");
    CHECK(updates[0].message->from_id == 5);
    CHECK(updates[0].message->text == std::optional<std::string>{"hola"});
    CHECK(updates[0].message->date == 1700000000);
    REQUIRE(updates[1].message.has_value());
    CHECK_FALSE(updates[1].message->text.has_value()); // Una foto.
    REQUIRE(updates[2].stop.has_value());
    CHECK(updates[2].stop->draft_id == 7);
    CHECK_FALSE(updates[3].message.has_value()); // Otro tipo: solo cuenta.

    const std::vector<Call> calls = h.server->calls("getUpdates");
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].url == "https://api.telegram.org/bot" + std::string{kToken} + "/getUpdates");
    CHECK(calls[0].body["timeout"] == 50);
    CHECK_FALSE(calls[0].body.contains("offset")); // El primero, sin offset.
    CHECK(calls[0].body["allowed_updates"] == json::array({"message", "stopped_message_generation"}));
    // El transporte espera más que el long polling.
    CHECK(calls[0].timeout > std::chrono::seconds{50});

    h.server->push_updates(json::array());
    REQUIRE(h.api.get_updates(14, std::chrono::seconds{50}).is_ok());
    CHECK(h.server->calls("getUpdates").back().body["offset"] == 14);
}

TEST_CASE("api: los errores de Telegram dan su ErrorKind", "[telegram][api]") {
    struct Case {
        int status;
        ErrorKind kind;
    };
    for (const Case& c : {Case{400, ErrorKind::InvalidRequest}, Case{401, ErrorKind::Auth},
                          Case{403, ErrorKind::Auth}, Case{404, ErrorKind::Auth},
                          Case{409, ErrorKind::Config}, Case{429, ErrorKind::RateLimited},
                          Case{500, ErrorKind::Server}, Case{502, ErrorKind::Server},
                          Case{418, ErrorKind::BadResponse}}) {
        ApiHarness h;
        h.server->set_respond([&c](const Call&) {
            return chatbot_telegram_test::failure(c.status, "Algo pasó");
        });
        const auto error = h.api.send_message(1, "hola");
        REQUIRE(error.has_value());
        CHECK(error->kind == c.kind);
        CHECK(error->http_status == c.status);
        CHECK(error->message.find("Algo pasó") != std::string::npos);
    }
    // 429 con parameters.retry_after.
    ApiHarness h;
    h.server->set_respond([](const Call&) {
        return chatbot_telegram_test::failure(429, "Too Many Requests: retry after 7", 7);
    });
    const auto limited = h.api.send_rich_message(1, "hola");
    REQUIRE(limited.has_value());
    CHECK(limited->kind == ErrorKind::RateLimited);
    CHECK(limited->retry_after == std::optional<std::chrono::seconds>{std::chrono::seconds{7}});
    // Cuerpo que no es JSON, sin red, timeout y cancelado.
    h.server->set_respond([](const Call&) {
        chatbot::HttpResponse response;
        response.status = 200;
        response.body = "<html>";
        return response;
    });
    CHECK(h.api.send_message(1, "x")->kind == ErrorKind::BadResponse);
    h.server->set_respond([](const Call&) { return chatbot_telegram_test::no_network(); });
    CHECK(h.api.send_message(1, "x")->kind == ErrorKind::Network);
    h.server->set_respond([](const Call&) {
        chatbot::HttpResponse response = chatbot_telegram_test::no_network("Operation timed out");
        response.timed_out = true;
        return response;
    });
    CHECK(h.api.send_message(1, "x")->kind == ErrorKind::Timeout);
    chatbot::CancelToken cancel;
    cancel.cancel();
    CHECK(h.api.send_message(1, "x", &cancel)->kind == ErrorKind::Cancelled);
}

TEST_CASE("api: el token nunca aparece en un error, por ningún camino", "[telegram][api][token]") {
    const std::string token{kToken};
    const std::string encoded = token.substr(0, token.find(':')) + "%3A" + token.substr(token.find(':') + 1);
    const std::string url = "https://api.telegram.org/bot" + token + "/sendMessage";
    std::vector<std::function<chatbot::HttpResponse(const Call&)>> answers = {
        // curl repite la URL en su mensaje.
        [url](const Call&) { return chatbot_telegram_test::no_network("Failed to connect to " + url); },
        [encoded](const Call&) {
            return chatbot_telegram_test::no_network("URL rejected: bot" + encoded);
        },
        // Redirección a la misma ruta (con el token) o a otra.
        [url](const Call&) {
            chatbot::HttpResponse response;
            response.status = 301;
            response.redirect_url = url;
            return response;
        },
        [token](const Call&) {
            chatbot::HttpResponse response;
            response.status = 302;
            response.redirect_url = "https://otro.example/bot" + token + "/sendMessage?x=1";
            response.body = "Moved";
            return response;
        },
        // Telegram repitiendo el token en la descripción.
        [token](const Call&) {
            return chatbot_telegram_test::failure(401, "Unauthorized: invalid token " + token);
        },
        [token](const Call&) {
            return chatbot_telegram_test::failure(400, "Bad Request: " + token.substr(token.find(':') + 1));
        },
        // Un cuerpo de error que no es JSON pero trae la URL.
        [url](const Call&) {
            chatbot::HttpResponse response;
            response.status = 502;
            response.body = "<html>Bad gateway for " + url + "</html>";
            return response;
        },
    };
    for (const auto& answer : answers) {
        ApiHarness h;
        h.server->set_respond(answer);
        const auto error = h.api.send_message(1, "hola");
        REQUIRE(error.has_value());
        INFO(error->message);
        CHECK_FALSE(has_token(error->message));
        // También al arrancar (getMe) y en el long polling.
        const auto me = h.api.get_me();
        REQUIRE(me.is_error());
        CHECK_FALSE(has_token(me.error().message));
    }
    // redact tapa las tres formas.
    ApiHarness h;
    const std::string redacted = h.api.redact("a " + token + " b " + encoded + " c " +
                                              token.substr(token.find(':') + 1));
    CHECK_FALSE(has_token(redacted));
    CHECK(redacted == "a <token> b <token> c <token>");
}

TEST_CASE("api: lo que se manda en cada método", "[telegram][api]") {
    ApiHarness h;
    REQUIRE_FALSE(h.api.send_message(5, "texto plano *sin* formato").has_value());
    REQUIRE_FALSE(h.api.send_rich_message(5, "**rico**").has_value());
    REQUIRE_FALSE(h.api.send_rich_message_draft(5, 9, "borrador").has_value());
    REQUIRE_FALSE(h.api.send_chat_action(5).has_value());
    REQUIRE_FALSE(h.api.set_my_commands(chatbot::telegram::bot_commands(true)).has_value());
    const std::vector<Call> calls = h.server->calls();
    REQUIRE(calls.size() == 5);
    CHECK(calls[0].method == "sendMessage");
    CHECK(calls[0].body["chat_id"] == 5);
    CHECK(calls[0].body["text"] == "texto plano *sin* formato");
    CHECK_FALSE(calls[0].body.contains("parse_mode"));
    CHECK(calls[0].body["link_preview_options"]["is_disabled"] == true);
    CHECK(calls[1].method == "sendRichMessage");
    CHECK(calls[1].body["rich_message"]["markdown"] == "**rico**");
    CHECK(calls[1].body["rich_message"]["skip_entity_detection"] == true);
    CHECK(calls[2].method == "sendRichMessageDraft");
    CHECK(calls[2].body["draft_id"] == 9);
    CHECK(calls[2].body["can_stop"] == true);
    CHECK(calls[2].body["keep_on_stop"] == true);
    CHECK(calls[3].method == "sendChatAction");
    CHECK(calls[3].body["action"] == "typing");
    CHECK(calls[4].method == "setMyCommands");
    const json commands = calls[4].body["commands"];
    REQUIRE(commands.size() == 4);
    CHECK(commands[0]["command"] == "start");
    CHECK(commands[3]["command"] == "buscar");
    for (const json& command : commands) {
        CHECK(command["description"].get<std::string>().size() <= 256);
    }
    CHECK(chatbot::telegram::bot_commands(false).size() == 3); // Sin búsqueda, sin /buscar.
    // Bytes inválidos no rompen el JSON.
    CHECK_FALSE(h.api.send_message(5, "mal \xFF byte").has_value());
}

TEST_CASE("arranque: token inválido es un error claro; sin red, espera", "[telegram][api]") {
    chatbot_bot_test::MemoryLog log;
    chatbot::CancelToken shutdown;
    {
        ApiHarness h;
        h.server->set_respond([](const Call&) {
            return chatbot_telegram_test::failure(401, "Unauthorized");
        });
        const auto result = chatbot::telegram::connect_to_telegram(h.api, log, shutdown);
        REQUIRE(result.is_error());
        CHECK(result.error().kind == ErrorKind::Auth);
        CHECK(result.error().message.find("@BotFather") != std::string::npos);
        CHECK_FALSE(has_token(result.error().message));
    }
    {
        ApiHarness h;
        int calls = 0;
        h.server->set_respond([&calls](const Call&) {
            return ++calls <= 4 ? chatbot_telegram_test::no_network()
                                : chatbot_telegram_test::ok(json{{"id", 42}, {"username", "pruebabot"}});
        });
        chatbot_test::FakeSleeper sleeper;
        const auto result = chatbot::telegram::connect_to_telegram(h.api, log, shutdown, &sleeper,
                                                                   std::chrono::seconds{4});
        REQUIRE(result.is_ok());
        CHECK(result.value().id == 42);
        CHECK(result.value().username == "pruebabot");
        CHECK(sleeper.sleeps == std::vector<std::chrono::milliseconds>{
                                    std::chrono::seconds{1}, std::chrono::seconds{2},
                                    std::chrono::seconds{4}, std::chrono::seconds{4}});
        CHECK(log.contains("sin_conexion"));
        CHECK(log.contains("conexion_recuperada fallos=4"));
    }
}
