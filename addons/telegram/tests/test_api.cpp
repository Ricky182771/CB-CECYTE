// Cliente de la Bot API (BotApi) con un Telegram falso: peticiones, updates,
// errores y que el token nunca salga en un mensaje.

#include "telegram_harness.hpp"

#include "chatbot/telegram/api.h"
#include "chatbot/telegram/startup.h"

#include "bot_harness.hpp"
#include "fake_transport.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

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
    REQUIRE(updates.size() == 5);
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
    CHECK_FALSE(updates[3].problem.has_value());
    // Sin update_id: se anota, no cuenta para el offset.
    CHECK(updates[4].update_id == 0);
    REQUIRE(updates[4].problem.has_value());
    CHECK(updates[4].problem->reason == "update_id_invalido");

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

TEST_CASE("api: Detener tal como lo manda Telegram, con draft_id en cadena",
          "[telegram][api][detener]") {
    ApiHarness h;
    h.server->push_updates(json::array({json::parse(chatbot_telegram_test::kRealStopUpdate)}));
    const auto result = h.api.get_updates(0, std::chrono::seconds{50});
    REQUIRE(result.is_ok());
    REQUIRE(result.value().size() == 1);
    const chatbot::telegram::Update& update = result.value()[0];
    CHECK(update.update_id == 449973635);
    CHECK_FALSE(update.problem.has_value());
    REQUIRE(update.stop.has_value());
    CHECK(update.stop->draft_id == 12345);
    CHECK(update.stop->chat_id == 6000000042);
    CHECK(update.stop->chat_type == "private");
}

TEST_CASE("api: Detener con draft_id numérico también sirve", "[telegram][api][detener]") {
    ApiHarness h;
    json update = json::parse(chatbot_telegram_test::kRealStopUpdate);
    update["stopped_message_generation"]["draft_id"] = 12345;
    h.server->push_updates(json::array({update}));
    const auto result = h.api.get_updates(0, std::chrono::seconds{50});
    REQUIRE(result.is_ok());
    REQUIRE(result.value().size() == 1);
    REQUIRE(result.value()[0].stop.has_value());
    CHECK(result.value()[0].stop->draft_id == 12345);
}

TEST_CASE("api: un Detener que no se entiende llega sin stop y con el motivo",
          "[telegram][api][detener]") {
    struct Case {
        const char* label;
        json draft;
        bool remove_draft = false;
        json chat_id = 6000000042;
        const char* reason = "draft_id_invalido";
    };
    const std::vector<Case> cases{
        {"cero", "0"},
        {"cero numérico", 0},
        {"negativo", "-5"},
        {"con espacio", " 12345"},
        {"decimal", "12345.0"},
        {"hexadecimal", "0x10"},
        {"fuera de rango", "9223372036854775808"},
        {"null", nullptr},
        {"booleano", true},
        {"objeto", json::object()},
        {"sin draft_id", "1", true},
        {"chat en cadena", "12345", false, "6000000042", nullptr},
        {"chat inválido", "12345", false, "abc", "chat_invalido"},
        {"chat null", "12345", false, nullptr, "chat_invalido"},
    };
    for (const Case& c : cases) {
        INFO(c.label);
        ApiHarness h;
        json update = json::parse(chatbot_telegram_test::kRealStopUpdate);
        update["stopped_message_generation"]["draft_id"] = c.draft;
        update["stopped_message_generation"]["chat"]["id"] = c.chat_id;
        if (c.remove_draft) {
            update["stopped_message_generation"].erase("draft_id");
        }
        h.server->push_updates(json::array({update}));
        const auto result = h.api.get_updates(0, std::chrono::seconds{50});
        REQUIRE(result.is_ok());
        REQUIRE(result.value().size() == 1);
        const chatbot::telegram::Update& parsed = result.value()[0];
        CHECK(parsed.update_id == 449973635);
        if (c.reason == nullptr) {
            CHECK_FALSE(parsed.problem.has_value());
            REQUIRE(parsed.stop.has_value());
            CHECK(parsed.stop->chat_id == 6000000042);
            continue;
        }
        CHECK_FALSE(parsed.stop.has_value());
        REQUIRE(parsed.problem.has_value());
        CHECK(parsed.problem->type == "stopped_message_generation");
        CHECK(parsed.problem->reason == c.reason);
    }
}

TEST_CASE("api: un update_id igual al máximo de int64_t es inválido, como número y como cadena",
          "[telegram][api]") {
    // El offset es update_id + 1: con el máximo desbordaría.
    constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
    ApiHarness h;
    json as_number = chatbot_telegram_test::text_update(1, 5, 5, "hola");
    as_number["update_id"] = kMax;
    json as_text = chatbot_telegram_test::text_update(1, 5, 5, "hola");
    as_text["update_id"] = std::to_string(kMax);
    h.server->push_updates(json::array(
        {as_number, as_text, chatbot_telegram_test::text_update(kMax - 1, 5, 5, "hola")}));
    const auto result = h.api.get_updates(0, std::chrono::seconds{50});
    REQUIRE(result.is_ok());
    const auto& updates = result.value();
    REQUIRE(updates.size() == 3);
    for (std::size_t i = 0; i < 2; ++i) {
        INFO(i);
        CHECK(updates[i].update_id == 0);
        CHECK_FALSE(updates[i].message.has_value());
        REQUIRE(updates[i].problem.has_value());
        CHECK(updates[i].problem->type == "message");
        CHECK(updates[i].problem->reason == "update_id_invalido");
    }
    // El anterior sí sirve.
    CHECK(updates[2].update_id == kMax - 1);
    CHECK_FALSE(updates[2].problem.has_value());
    CHECK(updates[2].message.has_value());
}

TEST_CASE("api: los enteros de un mensaje y el update_id pueden llegar como cadena",
          "[telegram][api]") {
    ApiHarness h;
    json as_text = chatbot_telegram_test::text_update(20, 5, 6, "hola", 1700000000);
    as_text["update_id"] = "20";
    as_text["message"]["chat"]["id"] = "5";
    as_text["message"]["from"]["id"] = "6";
    as_text["message"]["date"] = "1700000000";
    json bad_chat = chatbot_telegram_test::text_update(21, 5, 6, "hola");
    bad_chat["message"]["chat"]["id"] = "5 ";
    json bad_from = chatbot_telegram_test::text_update(22, 5, 6, "hola");
    bad_from["message"]["from"]["id"] = true;
    json bad_date = chatbot_telegram_test::text_update(23, 5, 6, "hola");
    bad_date["message"]["date"] = "ayer";
    json bad_id = chatbot_telegram_test::text_update(24, 5, 6, "hola");
    bad_id["update_id"] = "+24";
    h.server->push_updates(json::array({as_text, bad_chat, bad_from, bad_date, bad_id}));
    const auto result = h.api.get_updates(0, std::chrono::seconds{50});
    REQUIRE(result.is_ok());
    const auto& updates = result.value();
    REQUIRE(updates.size() == 5);

    CHECK(updates[0].update_id == 20);
    CHECK_FALSE(updates[0].problem.has_value());
    REQUIRE(updates[0].message.has_value());
    CHECK(updates[0].message->chat_id == 5);
    CHECK(updates[0].message->from_id == 6);
    CHECK(updates[0].message->date == 1700000000);

    const std::vector<std::string> reasons{"chat_invalido", "from_invalido", "date_invalido",
                                           "update_id_invalido"};
    for (std::size_t i = 0; i < reasons.size(); ++i) {
        INFO(reasons[i]);
        const chatbot::telegram::Update& update = updates[i + 1];
        CHECK_FALSE(update.message.has_value());
        REQUIRE(update.problem.has_value());
        CHECK(update.problem->type == "message");
        CHECK(update.problem->reason == reasons[i]);
    }
    CHECK(updates[4].update_id == 0);
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
