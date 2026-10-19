// Long polling (Poller): offset, qué se le pasa al bot, chats no privados,
// reconexión sin llenar el registro, 429 y apagado.

#include "telegram_harness.hpp"

#include "chatbot/telegram/poller.h"

#include "bot_harness.hpp"
#include "fake_transport.hpp"
#include "performance.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using chatbot::telegram::BotApi;
using chatbot::telegram::Poller;
using chatbot::telegram::PollerHandlers;
using chatbot::telegram::PollerOptions;
using chatbot_telegram_test::FakeTelegram;
using chatbot_telegram_test::kToken;
using nlohmann::json;
using namespace std::chrono_literals;

struct PollerHarness {
    std::shared_ptr<FakeTelegram> server = std::make_shared<FakeTelegram>();
    BotApi api{std::string{kToken}, chatbot_telegram_test::transports(server)};
    chatbot_bot_test::MemoryLog log;
    chatbot::CancelToken shutdown;
    chatbot_test::FakeSleeper sleeper;

    std::mutex mutex;
    std::vector<chatbot::bot::IncomingMessage> messages;
    std::vector<std::pair<chatbot::bot::ChatId, chatbot::bot::ReplyId>> stops;
    int idle = 0;

    Poller make(std::chrono::seconds max_backoff = 60s) {
        PollerOptions options;
        options.max_backoff = max_backoff;
        options.sleeper = &sleeper;
        return Poller{api,
                      PollerHandlers{[this](const chatbot::bot::IncomingMessage& message) {
                                         const std::lock_guard<std::mutex> lock(mutex);
                                         messages.push_back(message);
                                     },
                                     [this](chatbot::bot::ChatId chat, chatbot::bot::ReplyId reply) {
                                         const std::lock_guard<std::mutex> lock(mutex);
                                         stops.emplace_back(chat, reply);
                                     },
                                     [this] { ++idle; }},
                      log, shutdown, options};
    }
};

} // namespace

TEST_CASE("poller: el offset se recalcula con cada respuesta", "[telegram][poller]") {
    PollerHarness h;
    Poller poller = h.make();
    h.server->push_updates(json::array({chatbot_telegram_test::text_update(100, 5, 5, "a"),
                                        chatbot_telegram_test::text_update(102, 5, 5, "b")}));
    h.server->push_updates(json::array());
    h.server->push_updates(json::array({chatbot_telegram_test::text_update(103, 6, 6, "c")}));
    for (int i = 0; i < 3; ++i) {
        REQUIRE_FALSE(poller.poll_once().has_value());
    }
    const auto calls = h.server->calls("getUpdates");
    REQUIRE(calls.size() == 3);
    CHECK_FALSE(calls[0].body.contains("offset"));
    CHECK(calls[1].body["offset"] == 103);
    CHECK(calls[2].body["offset"] == 103);
    CHECK(poller.offset() == 104);
    CHECK(h.idle == 3);
    REQUIRE(h.messages.size() == 3);
    CHECK(h.messages[2].chat == 6);
}

TEST_CASE("poller: mensajes privados al bot; grupos, canales y bots no", "[telegram][poller]") {
    PollerHarness h;
    Poller poller = h.make();
    json bot_message = chatbot_telegram_test::text_update(5, 7, 7, "soy bot");
    bot_message["message"]["from"]["is_bot"] = true;
    h.server->push_updates(json::array(
        {chatbot_telegram_test::text_update(1, 7, 7, "hola", 1760000000),
         chatbot_telegram_test::text_update(2, 7, 7, ""),
         chatbot_telegram_test::text_update(3, -100123, 7, "en grupo", 0, "supergroup"),
         chatbot_telegram_test::text_update(4, -100123, 8, "otra vez", 0, "supergroup"),
         chatbot_telegram_test::text_update(6, -100999, 7, "canal", 0, "channel"), bot_message,
         chatbot_telegram_test::stop_update(7, 7, 33)}));
    REQUIRE_FALSE(poller.poll_once().has_value());
    REQUIRE(h.messages.size() == 2);
    CHECK(h.messages[0].platform == "telegram");
    CHECK(h.messages[0].chat == 7);
    CHECK(h.messages[0].user == 7);
    CHECK(h.messages[0].text == "hola");
    REQUIRE(h.messages[0].sent_at.has_value());
    CHECK(std::chrono::system_clock::to_time_t(*h.messages[0].sent_at) == 1760000000);
    CHECK(h.messages[1].text.empty()); // Una foto: el bot contesta kTextOnly.
    CHECK(h.messages[0].display_name.empty()); // Los nombres no se usan.
    REQUIRE(h.stops.size() == 1);
    CHECK(h.stops[0] == std::pair<chatbot::bot::ChatId, chatbot::bot::ReplyId>{7, 33});
    // Cada grupo se anota una vez, sin el texto.
    std::size_t noted = 0;
    for (const std::string& line : h.log.lines()) {
        noted += line.find("chat_no_privado chat=-100123") != std::string::npos ? 1 : 0;
        CHECK(line.find("en grupo") == std::string::npos);
    }
    CHECK(noted == 1);
    CHECK(h.log.contains("chat_no_privado chat=-100999"));
}

TEST_CASE("poller: sin red espera cada vez más y lo anota solo dos veces",
          "[telegram][poller][red]") {
    PollerHarness h;
    Poller poller = h.make(8s);
    for (int i = 0; i < 6; ++i) {
        h.server->push_updates(chatbot_telegram_test::no_network());
    }
    h.server->push_updates(chatbot_telegram_test::failure(502, "Bad Gateway"));
    h.server->push_updates(json::array({chatbot_telegram_test::text_update(1, 7, 7, "volví")}));
    for (int i = 0; i < 8; ++i) {
        REQUIRE_FALSE(poller.poll_once().has_value());
    }
    CHECK(h.sleeper.sleeps == std::vector<std::chrono::milliseconds>{1s, 2s, 4s, 8s, 8s, 8s, 8s});
    std::size_t lost = 0;
    std::size_t back = 0;
    for (const std::string& line : h.log.lines()) {
        lost += line.find("sin_conexion") != std::string::npos ? 1 : 0;
        back += line.find("conexion_recuperada fallos=7") != std::string::npos ? 1 : 0;
    }
    CHECK(lost == 1);
    CHECK(back == 1);
    CHECK(h.messages.size() == 1);

    // Si se vuelve a ir, otra vez desde 1 s.
    h.sleeper.sleeps.clear();
    h.server->push_updates(chatbot_telegram_test::no_network());
    REQUIRE_FALSE(poller.poll_once().has_value());
    CHECK(h.sleeper.sleeps == std::vector<std::chrono::milliseconds>{1s});
}

TEST_CASE("poller: 429 espera retry_after; token inválido termina", "[telegram][poller]") {
    PollerHarness h;
    Poller poller = h.make();
    h.server->push_updates(chatbot_telegram_test::failure(429, "Too Many Requests", 9));
    REQUIRE_FALSE(poller.poll_once().has_value());
    CHECK(h.sleeper.sleeps == std::vector<std::chrono::milliseconds>{9s});
    CHECK_FALSE(h.log.contains("sin_conexion"));

    h.server->push_updates(chatbot_telegram_test::failure(401, "Unauthorized"));
    const auto fatal = poller.poll_once();
    REQUIRE(fatal.has_value());
    CHECK(fatal->kind == chatbot::ErrorKind::Auth);
    CHECK(h.log.contains("telegram_token_invalido http=401"));
}

TEST_CASE("poller: el apagado corta el long polling en menos de 2 s", "[telegram][poller]") {
    PollerHarness h;
    PollerOptions options; // Espera real, cortada con el token.
    Poller poller{h.api, PollerHandlers{}, h.log, h.shutdown, options};
    std::thread thread{[&] { (void)poller.run(); }};
    REQUIRE(h.server->wait_for("getUpdates", 1));
    const auto started = std::chrono::steady_clock::now();
    h.shutdown.cancel();
    thread.join();
    chatbot_test::report_latency("apagado del long polling", std::chrono::steady_clock::now() - started);
}
