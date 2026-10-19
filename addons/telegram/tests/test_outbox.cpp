// TelegramOutbox: mensajes enriquecidos y planos, reintentos con
// retry_after, vista previa (pausa, keepalive, 429) y "escribiendo…".

#include "telegram_harness.hpp"

#include "chatbot/telegram/outbox.h"
#include "chatbot/telegram/rich_markdown.h"

#include "bot_harness.hpp"
#include "fake_transport.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory>
#include <string>

namespace {

using chatbot::bot::TextFormat;
using chatbot::telegram::BotApi;
using chatbot::telegram::OutboxOptions;
using chatbot::telegram::TelegramOutbox;
using chatbot_telegram_test::Call;
using chatbot_telegram_test::FakeTelegram;
using chatbot_telegram_test::kToken;
using nlohmann::json;
using namespace std::chrono_literals;

struct OutboxHarness {
    std::shared_ptr<FakeTelegram> server = std::make_shared<FakeTelegram>();
    BotApi api{std::string{kToken}, chatbot_telegram_test::transports(server)};
    chatbot_bot_test::MemoryLog log;
    chatbot::CancelToken shutdown;
    chatbot_test::FakeSleeper sleeper;
    std::unique_ptr<TelegramOutbox> outbox;

    explicit OutboxHarness(bool details = false) {
        OutboxOptions options;
        options.log_error_details = details;
        options.sleeper = &sleeper;
        outbox = std::make_unique<TelegramOutbox>(api, log, shutdown, options);
    }
};

} // namespace

TEST_CASE("outbox: Markdown va saneado por sendRichMessage; texto plano por sendMessage",
          "[telegram][outbox]") {
    OutboxHarness h;
    CHECK(h.outbox->send(5, "Hola <b>x</b> y $5", TextFormat::Markdown));
    CHECK(h.outbox->send(5, "Fuentes:\n[1] <b>x</b>", TextFormat::Plain));
    const auto calls = h.server->calls();
    REQUIRE(calls.size() == 2);
    CHECK(calls[0].method == "sendRichMessage");
    CHECK(calls[0].body["rich_message"]["markdown"] ==
          chatbot::telegram::to_rich_markdown("Hola <b>x</b> y $5"));
    CHECK(calls[1].method == "sendMessage");
    CHECK(calls[1].body["text"] == "Fuentes:\n[1] <b>x</b>");
    // Los topes: 4096 UTF-16 en plano, el enriquecido medido saneado.
    CHECK(h.outbox->message_limit(TextFormat::Plain).max_size == 4096);
    CHECK(h.outbox->message_limit(TextFormat::Markdown).max_size == 32768);
    CHECK(h.outbox->message_limit(TextFormat::Markdown).max_blocks == 500);
}

TEST_CASE("outbox: 429 espera retry_after y reintenta", "[telegram][outbox]") {
    OutboxHarness h;
    int calls = 0;
    h.server->set_respond([&calls](const Call&) {
        return ++calls == 1 ? chatbot_telegram_test::failure(429, "Too Many Requests", 3)
                            : chatbot_telegram_test::ok(json{{"message_id", 1}});
    });
    CHECK(h.outbox->send(5, "hola", TextFormat::Plain));
    CHECK(h.sleeper.sleeps == std::vector<std::chrono::milliseconds>{3s});
    CHECK(calls == 2);
    // Sin red: 1 s, 2 s y se rinde a los 3 intentos.
    h.sleeper.sleeps.clear();
    h.server->set_respond([](const Call&) { return chatbot_telegram_test::no_network(); });
    CHECK_FALSE(h.outbox->send(5, "hola", TextFormat::Plain));
    CHECK(h.sleeper.sleeps == std::vector<std::chrono::milliseconds>{1s, 2s});
    CHECK(h.log.contains("telegram_error metodo=sendMessage chat=5 http=0"));
}

TEST_CASE("outbox: un enriquecido rechazado devuelve false sin reintentar", "[telegram][outbox]") {
    OutboxHarness h;
    h.server->set_respond([](const Call& call) {
        return call.method == "sendRichMessage"
                   ? chatbot_telegram_test::failure(400, "Bad Request: can't parse rich message")
                   : chatbot_telegram_test::ok();
    });
    CHECK_FALSE(h.outbox->send(5, "**hola**", TextFormat::Markdown));
    CHECK(h.server->calls("sendRichMessage").size() == 1);
    CHECK(h.sleeper.sleeps.empty());
    CHECK(h.log.contains("telegram_error metodo=sendRichMessage chat=5 http=400"));
    CHECK_FALSE(h.log.contains("can't parse")); // Sin la descripción por defecto.
}

TEST_CASE("outbox: con depuración, la descripción va al registro sin el token",
          "[telegram][outbox][token]") {
    OutboxHarness h{true};
    const std::string token{kToken};
    h.server->set_respond([token](const Call&) {
        return chatbot_telegram_test::failure(400, "Bad Request: token " + token + " no");
    });
    CHECK_FALSE(h.outbox->send(5, "x", TextFormat::Plain));
    CHECK(h.log.contains("detalle="));
    CHECK(h.log.contains("<token>"));
    CHECK_FALSE(h.log.contains(token));
    CHECK_FALSE(h.log.contains(token.substr(token.find(':') + 1)));
}

TEST_CASE("outbox: la vista previa, su pausa y su keepalive", "[telegram][outbox][vista]") {
    OutboxHarness h;
    std::unique_ptr<chatbot::bot::Draft> draft = h.outbox->open_draft(5, 77);
    REQUIRE(draft != nullptr);
    CHECK(h.outbox->open_draft(-100123, 78) == nullptr); // Grupo: no hay vistas previas.
    CHECK(draft->status("Pensando…"));
    CHECK(draft->update("Hola **mundo** <b>"));
    CHECK(draft->update("   ")); // Nada que mostrar: no se manda.
    auto drafts = h.server->calls("sendRichMessageDraft");
    REQUIRE(drafts.size() == 2);
    CHECK(drafts[0].body["draft_id"] == 77);
    CHECK(drafts[0].body["rich_message"]["markdown"] == "<tg-thinking>Pensando…</tg-thinking>");
    CHECK(drafts[1].body["rich_message"]["markdown"] ==
          chatbot::telegram::to_rich_markdown("Hola **mundo** <b>"));

    // Sin cambios en 20 s: se vuelve a mandar (las vistas previas duran ~30 s).
    const auto now = std::chrono::steady_clock::now();
    h.outbox->tick(now + 5s);
    CHECK(h.server->calls("sendRichMessageDraft").size() == 2);
    h.outbox->tick(now + 21s);
    drafts = h.server->calls("sendRichMessageDraft");
    REQUIRE(drafts.size() == 3);
    CHECK(drafts[2].body == drafts[1].body);

    // Después de un mensaje al chat, la vista previa vieja no vuelve.
    CHECK(h.outbox->send(5, "respuesta", TextFormat::Markdown));
    h.outbox->tick(now + 60s);
    CHECK(h.server->calls("sendRichMessageDraft").size() == 3);
    // Hasta el siguiente update.
    CHECK(draft->update("más texto"));
    CHECK(h.server->calls("sendRichMessageDraft").size() == 4);
    draft.reset();
    h.outbox->tick(now + 120s);
    CHECK(h.server->calls("sendRichMessageDraft").size() == 4);
}

TEST_CASE("outbox: un 429 en la vista previa la pausa sin esperar", "[telegram][outbox][vista]") {
    OutboxHarness h;
    std::unique_ptr<chatbot::bot::Draft> draft = h.outbox->open_draft(5, 1);
    h.server->set_respond([](const Call&) {
        return chatbot_telegram_test::failure(429, "Too Many Requests", 30);
    });
    CHECK_FALSE(draft->update("uno"));
    h.server->set_respond([](const Call&) { return chatbot_telegram_test::ok(); });
    CHECK(draft->update("uno dos")); // Bloqueada: se guarda, no se manda.
    CHECK(h.server->calls("sendRichMessageDraft").size() == 1);
    CHECK(h.sleeper.sleeps.empty()); // El hilo de trabajo nunca espera por una vista previa.
    // Pasado el retry_after, el hilo del Outbox manda la última.
    h.outbox->tick(std::chrono::steady_clock::now() + 31s);
    const auto drafts = h.server->calls("sendRichMessageDraft");
    REQUIRE(drafts.size() == 2);
    CHECK(drafts[1].body["rich_message"]["markdown"] == "uno dos");
}

TEST_CASE("outbox: escribiendo… se repite mientras siga encendido", "[telegram][outbox]") {
    OutboxHarness h;
    h.outbox->set_typing(5, true);
    CHECK(h.server->calls("sendChatAction").size() == 1);
    const auto now = std::chrono::steady_clock::now();
    h.outbox->tick(now + 1s);
    CHECK(h.server->calls("sendChatAction").size() == 1);
    h.outbox->tick(now + 5s);
    CHECK(h.server->calls("sendChatAction").size() == 2);
    h.outbox->set_typing(5, false);
    h.outbox->tick(now + 20s);
    CHECK(h.server->calls("sendChatAction").size() == 2);
}

TEST_CASE("outbox: el hilo propio arranca y se detiene", "[telegram][outbox]") {
    OutboxHarness h;
    h.outbox->start();
    h.outbox->start(); // Una segunda vez no hace nada.
    h.outbox->stop();
    h.outbox->stop();
}
