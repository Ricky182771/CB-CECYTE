// De punta a punta, sin red: Poller + bot::Bot + TelegramOutbox contra un
// Telegram falso y un modelo falso.

#include "telegram_harness.hpp"

#include "chatbot/telegram/outbox.h"
#include "chatbot/telegram/poller.h"
#include "chatbot/telegram/rich_markdown.h"

#include "chatbot/bot/bot.h"
#include "chatbot/bot/replies.h"

#include "bot_harness.hpp"
#include "fake_transport.hpp"
#include "performance.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <ctime>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using chatbot::bot::TextFormat;
using chatbot::telegram::BotApi;
using chatbot::telegram::OutboxOptions;
using chatbot::telegram::Poller;
using chatbot::telegram::PollerHandlers;
using chatbot::telegram::TelegramOutbox;
using chatbot_telegram_test::Call;
using chatbot_telegram_test::FakeTelegram;
using chatbot_telegram_test::kToken;
using nlohmann::json;
using namespace std::chrono_literals;

constexpr const char* kModelKey = "clave-ficticia-de-prueba"; // La de chatbot_test::test_config.

/// Sin espacios: lo que debe conservarse al partir.
std::string squeeze(std::string_view text) {
    std::string out;
    for (const char c : text) {
        if (c != ' ' && c != '\n') {
            out.push_back(c);
        }
    }
    return out;
}

struct World {
    std::shared_ptr<FakeTelegram> server = std::make_shared<FakeTelegram>();
    BotApi api{std::string{kToken}, chatbot_telegram_test::transports(server)};
    chatbot_bot_test::MemoryLog log;
    chatbot::CancelToken shutdown;
    chatbot_test::FakeSleeper sleeper;
    std::unique_ptr<TelegramOutbox> outbox;
    chatbot::bot::SystemClock clock;
    std::unique_ptr<chatbot::bot::Bot> bot;
    std::unique_ptr<Poller> poller;
    std::thread polling;

    World() {
        OutboxOptions options;
        options.sleeper = &sleeper;
        outbox = std::make_unique<TelegramOutbox>(api, log, shutdown, options);
    }

    ~World() { stop(); }
    World(const World&) = delete;
    World& operator=(const World&) = delete;

    /// first_reply: el primer draft_id (fijo, para que las pruebas sean
    /// deterministas).
    void start(chatbot::bot::BotConfig config, chatbot::bot::ClientFactory clients,
               chatbot::bot::ReplyId first_reply = 1) {
        chatbot::bot::BotDependencies dependencies{std::move(clients), {}, *outbox, clock, log};
        dependencies.first_reply = [first_reply] { return first_reply; };
        bot = std::make_unique<chatbot::bot::Bot>(chatbot_bot_test::valid(std::move(config)),
                                                  std::move(dependencies));
        poller = std::make_unique<Poller>(
            api,
            PollerHandlers{[this](const chatbot::bot::IncomingMessage& m) { bot->handle(m); },
                           [this](chatbot::bot::ChatId chat, chatbot::bot::ReplyId reply) {
                               (void)bot->stop(chat, reply);
                           },
                           [this] { bot->expire_idle(); }},
            log, shutdown);
        polling = std::thread{[this] { (void)poller->run(); }};
    }

    /// Un mensaje privado de texto, de ahora.
    void say(std::int64_t id, std::int64_t chat, const std::string& text) {
        server->push_updates(json::array(
            {chatbot_telegram_test::text_update(id, chat, chat, text, std::time(nullptr))}));
    }

    bool idle() {
        // Espera a que el poller tome lo pendiente y el bot termine.
        std::this_thread::sleep_for(50ms);
        return bot->wait_idle(chatbot_test::kThreadTimeout);
    }

    void stop() {
        shutdown.cancel();
        if (bot) {
            bot->shutdown();
        }
        if (polling.joinable()) {
            polling.join();
        }
        outbox->stop();
    }

    /// Lo que salió como mensaje (no vistas previas) a un chat.
    std::vector<Call> messages(std::int64_t chat) const {
        std::vector<Call> out;
        for (const Call& call : server->calls()) {
            if ((call.method == "sendRichMessage" || call.method == "sendMessage") &&
                call.body["chat_id"] == chat) {
                out.push_back(call);
            }
        }
        return out;
    }

    static std::string text_of(const Call& call) {
        return call.method == "sendMessage" ? call.body["text"].get<std::string>()
                                            : call.body["rich_message"]["markdown"].get<std::string>();
    }
};

chatbot::bot::BotConfig config_for(std::vector<chatbot::bot::UserId> users) {
    chatbot::bot::BotConfig config = chatbot_bot_test::test_bot_config();
    config.allowed_users = std::move(users);
    config.draft_interval = 250ms;
    return config;
}

chatbot::bot::ClientFactory answering(std::vector<std::string> pieces) {
    auto recorder = std::make_shared<chatbot_bot_test::Recorder>();
    return chatbot_bot_test::scripted_clients(recorder, [pieces](const auto&) {
        return chatbot_bot_test::ok_stream(pieces);
    });
}

} // namespace

TEST_CASE("telegram: respuesta con vista previa y mensaje final", "[telegram][integracion]") {
    World w;
    w.start(config_for({5}), chatbot_bot_test::slow_clients(12, 60ms));
    w.say(1, 5, "hola");
    REQUIRE(w.idle());
    const auto drafts = w.server->calls("sendRichMessageDraft");
    REQUIRE(drafts.size() >= 2);
    CHECK(drafts.front().body["rich_message"]["markdown"] == "<tg-thinking>Pensando…</tg-thinking>");
    CHECK(drafts.front().body["can_stop"] == true);
    const auto final_messages = w.messages(5);
    REQUIRE(final_messages.size() == 1);
    CHECK(final_messages[0].method == "sendRichMessage");
    CHECK(World::text_of(final_messages[0]) == "eco: hola...........");
    // El mensaje final llega después de todas las vistas previas.
    const auto calls = w.server->calls();
    std::size_t last_draft = 0;
    std::size_t final_at = 0;
    for (std::size_t i = 0; i < calls.size(); ++i) {
        if (calls[i].method == "sendRichMessageDraft") {
            last_draft = i;
        }
        if (calls[i].method == "sendRichMessage") {
            final_at = i;
        }
    }
    CHECK(final_at > last_draft);
}

TEST_CASE("telegram: una respuesta más larga que un mensaje enriquecido se parte sin perder nada",
          "[telegram][integracion][largo]") {
    SECTION("por caracteres") {
        World w;
        std::string answer;
        std::vector<std::string> pieces;
        for (int i = 0; i < 1500; ++i) {
            const std::string paragraph =
                "Párrafo " + std::to_string(i) + " con algo de texto para llenar <espacio>.\n\n";
            answer += paragraph;
            pieces.push_back(paragraph);
        }
        REQUIRE(answer.size() > chatbot::telegram::kRichMaxBytes * 2);
        w.start(config_for({5}), answering(pieces));
        w.say(1, 5, "dame mucho texto");
        REQUIRE(w.idle());
        const auto sent = w.messages(5);
        REQUIRE(sent.size() >= 3);
        std::string joined;
        for (const Call& call : sent) {
            CHECK(call.method == "sendRichMessage");
            const std::string markdown = World::text_of(call);
            CHECK(markdown.size() <= chatbot::telegram::kRichMaxBytes);
            CHECK(chatbot::telegram::rich_block_count(markdown) <= chatbot::telegram::kRichMaxBlocks);
            joined += markdown + "\n";
        }
        CHECK(squeeze(joined) == squeeze(chatbot::telegram::to_rich_markdown(answer)));
    }
    SECTION("por bloques") {
        World w;
        std::string answer;
        std::vector<std::string> pieces;
        for (int i = 0; i < 700; ++i) {
            const std::string item = "- elemento " + std::to_string(i) + "\n";
            answer += item;
            pieces.push_back(item);
        }
        REQUIRE(answer.size() < chatbot::telegram::kRichMaxBytes); // Cabe por caracteres.
        w.start(config_for({5}), answering(pieces));
        w.say(1, 5, "una lista larga");
        REQUIRE(w.idle());
        const auto sent = w.messages(5);
        REQUIRE(sent.size() >= 2);
        std::string joined;
        for (const Call& call : sent) {
            const std::string markdown = World::text_of(call);
            CHECK(chatbot::telegram::rich_block_count(markdown) <= chatbot::telegram::kRichMaxBlocks);
            joined += markdown + "\n";
        }
        CHECK(squeeze(joined) == squeeze(answer));
    }
}

TEST_CASE("telegram: un enriquecido rechazado sale como texto plano partido a 4096",
          "[telegram][integracion][respaldo]") {
    World w;
    w.server->set_respond([](const Call& call) {
        return call.method == "sendRichMessage"
                   ? chatbot_telegram_test::failure(400, "Bad Request: can't parse rich message")
                   : chatbot_telegram_test::ok(json{{"message_id", 1}});
    });
    std::string answer;
    for (int i = 0; i < 400; ++i) {
        answer += "renglón " + std::to_string(i) + " con acentos y emoji 👍\n";
    }
    REQUIRE(chatbot::bot::utf16_length(answer) > 4096 * 2);
    w.start(config_for({5}), answering({answer}));
    w.say(1, 5, "hola");
    REQUIRE(w.idle());
    const auto sent = w.messages(5);
    std::string joined;
    std::size_t plain = 0;
    for (const Call& call : sent) {
        if (call.method == "sendMessage") {
            ++plain;
            const std::string text = World::text_of(call);
            CHECK(chatbot::bot::utf16_length(text) <= 4096);
            joined += text + "\n";
        }
    }
    CHECK(plain >= 3);
    CHECK(squeeze(joined) == squeeze(answer));
    CHECK(w.log.contains("enviado_como_texto chat=5"));
}

namespace {

/// El draft_id de la primera vista previa a chat cuyo markdown contiene
/// text (0 si no llegó en unos segundos).
std::int64_t wait_for_draft(const World& w, std::int64_t chat, std::string_view text) {
    for (int i = 0; i < 250; ++i) {
        for (const Call& call : w.server->calls("sendRichMessageDraft")) {
            if (call.body["chat_id"] == chat &&
                call.body["rich_message"]["markdown"].get<std::string>().find(text) !=
                    std::string::npos) {
                return call.body["draft_id"].get<std::int64_t>();
            }
        }
        std::this_thread::sleep_for(20ms);
    }
    return 0;
}

/// Lo que se mandó al modelo en la última petición (sistema, historial y
/// la pregunta).
std::vector<chatbot::Message> last_request(const chatbot_bot_test::Recorder& recorder) {
    const auto requests = recorder.requests();
    REQUIRE_FALSE(requests.empty());
    return chatbot_test::sent_messages_of(requests.back());
}

bool has_text(const std::vector<Call>& calls, std::string_view text) {
    for (const Call& call : calls) {
        if (World::text_of(call) == text) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE("telegram: Detener a la mitad (como lo manda Telegram) cancela en menos de 2 s",
          "[telegram][integracion][detener]") {
    World w;
    // La primera respuesta es lenta y larga; las siguientes, inmediatas.
    auto recorder = std::make_shared<chatbot_bot_test::Recorder>();
    auto slow = std::make_shared<std::atomic<bool>>(true);
    w.start(config_for({5}), [slow, recorder]() -> std::unique_ptr<chatbot::ChatClient> {
        if (slow->exchange(false)) {
            return std::make_unique<chatbot::ChatClient>(
                chatbot_test::test_config(),
                std::make_unique<chatbot_bot_test::SlowTransport>(100000, 30ms),
                std::make_unique<chatbot_test::FakeSleeper>());
        }
        return chatbot_bot_test::scripted_clients(recorder)();
    });
    w.say(1, 5, "cuéntame algo largo");
    const std::int64_t draft_id = wait_for_draft(w, 5, "eco");
    REQUIRE(draft_id != 0);

    // El update real: draft_id como cadena.
    const auto started = std::chrono::steady_clock::now();
    w.server->push_updates(json::array({chatbot_telegram_test::real_stop_update(2, 5, draft_id)}));
    REQUIRE(w.server->wait_for("sendMessage", 1));
    chatbot_test::report_latency("Detener desde Telegram", std::chrono::steady_clock::now() - started);
    REQUIRE(w.idle());
    const auto sent = w.messages(5);
    REQUIRE(sent.size() == 2);
    CHECK(sent[0].method == "sendRichMessage");
    CHECK(World::text_of(sent[0]).rfind("eco:", 0) == 0);
    CHECK(World::text_of(sent[1]) == chatbot::bot::kStoppedNote);
    CHECK(w.log.contains("detener chat=5"));
    CHECK(w.log.contains("detenida chat=5 usuario=5"));
    CHECK_FALSE(w.log.contains("update_invalido"));
    CHECK_FALSE(w.log.contains("detener_sin_respuesta"));

    // El siguiente mensaje se responde normal, sin "Espera…", y la
    // detenida no entró al historial.
    w.say(3, 5, "segunda");
    REQUIRE(w.idle());
    const auto after = w.messages(5);
    CHECK_FALSE(has_text(after, chatbot::bot::kWaitPrevious));
    REQUIRE(after.size() == 3);
    CHECK(World::text_of(after[2]) == "eco: segunda");
    const std::vector<chatbot::Message> history = last_request(*recorder);
    REQUIRE(history.size() == 2); // Sistema y la segunda pregunta.
    CHECK(history[0].role == chatbot::Role::System);
    CHECK(history[1].content == "segunda");
}

TEST_CASE("telegram: Detener durante \"Pensando…\" manda solo el aviso",
          "[telegram][integracion][detener]") {
    World w;
    auto gate = std::make_shared<chatbot_bot_test::Gate>();
    auto recorder = std::make_shared<chatbot_bot_test::Recorder>();
    w.start(config_for({5}), chatbot_bot_test::gated_clients(gate, recorder));
    w.say(1, 5, "hola");
    REQUIRE(gate->wait_inside(1)); // El modelo aún no manda nada.
    const std::int64_t draft_id = wait_for_draft(w, 5, "Pensando");
    REQUIRE(draft_id != 0);

    const auto started = std::chrono::steady_clock::now();
    w.server->push_updates(json::array({chatbot_telegram_test::real_stop_update(2, 5, draft_id)}));
    REQUIRE(w.server->wait_for("sendMessage", 1));
    chatbot_test::report_latency("Detener en \"Pensando…\"",
                                 std::chrono::steady_clock::now() - started);
    REQUIRE(w.idle());
    const auto sent = w.messages(5);
    REQUIRE(sent.size() == 1);
    CHECK(World::text_of(sent[0]) == chatbot::bot::kStoppedEmpty);
    CHECK(w.log.contains("detener chat=5"));
    CHECK(w.log.contains("detenida chat=5 usuario=5"));

    gate->open();
    w.say(3, 5, "otra");
    REQUIRE(w.idle());
    const auto after = w.messages(5);
    CHECK_FALSE(has_text(after, chatbot::bot::kWaitPrevious));
    REQUIRE(after.size() == 2);
    CHECK(World::text_of(after[1]) == "eco: otra");
    const std::vector<chatbot::Message> history = last_request(*recorder);
    REQUIRE(history.size() == 2);
    CHECK(history[1].content == "otra");
}

TEST_CASE("telegram: Detener con el draft_id del tope y con el 1 que le sigue",
          "[telegram][integracion][detener]") {
    World w;
    auto gate = std::make_shared<chatbot_bot_test::Gate>();
    auto recorder = std::make_shared<chatbot_bot_test::Recorder>();
    w.start(config_for({5}), chatbot_bot_test::gated_clients(gate, recorder),
            chatbot::bot::kMaxReplyId);
    std::int64_t update_id = 1;
    std::size_t stopped = 0;
    for (const std::int64_t expected : {chatbot::bot::kMaxReplyId, std::int64_t{1}}) {
        const std::size_t drafts_before = w.server->calls("sendRichMessageDraft").size();
        w.say(update_id++, 5, "hola");
        REQUIRE(gate->wait_inside(1));
        std::int64_t draft_id = 0;
        for (int i = 0; i < 250 && draft_id == 0; ++i) {
            const auto drafts = w.server->calls("sendRichMessageDraft");
            if (drafts.size() > drafts_before) {
                draft_id = drafts[drafts_before].body["draft_id"].get<std::int64_t>();
            } else {
                std::this_thread::sleep_for(20ms);
            }
        }
        CHECK(draft_id == expected);
        // Como lo manda Telegram: draft_id en cadena ("2147483647", "1").
        w.server->push_updates(
            json::array({chatbot_telegram_test::real_stop_update(update_id++, 5, draft_id)}));
        ++stopped;
        REQUIRE(w.server->wait_for("sendMessage", stopped));
        REQUIRE(w.idle());
    }
    const auto sent = w.messages(5);
    REQUIRE(sent.size() == 2);
    CHECK(World::text_of(sent[0]) == chatbot::bot::kStoppedEmpty);
    CHECK(World::text_of(sent[1]) == chatbot::bot::kStoppedEmpty);
    CHECK_FALSE(w.log.contains("update_invalido"));
    CHECK_FALSE(w.log.contains("detener_sin_respuesta"));
}

TEST_CASE("telegram: Detener cuando la respuesta ya terminó solo se anota",
          "[telegram][integracion][detener]") {
    World w;
    w.start(config_for({5}), answering({"respuesta"}));
    w.say(1, 5, "hola");
    REQUIRE(w.idle());
    const std::int64_t draft_id = wait_for_draft(w, 5, "Pensando");
    REQUIRE(draft_id != 0);
    const std::size_t before = w.server->calls().size();
    REQUIRE(w.messages(5).size() == 1);

    w.server->push_updates(json::array({chatbot_telegram_test::real_stop_update(2, 5, draft_id)}));
    bool noted = false;
    for (int i = 0; i < 250 && !noted; ++i) {
        noted = w.log.contains("detener_sin_respuesta chat=5");
        std::this_thread::sleep_for(10ms);
    }
    REQUIRE(noted);
    CHECK(w.log.level_of("detener_sin_respuesta chat=5") == chatbot::bot::LogLevel::Info);
    REQUIRE(w.idle());
    CHECK(w.messages(5).size() == 1); // Nada más al chat.
    std::size_t sent_after = 0;
    const auto calls = w.server->calls();
    for (std::size_t i = before; i < calls.size(); ++i) {
        sent_after += calls[i].method != "getUpdates" ? 1 : 0;
    }
    CHECK(sent_after == 0);
    CHECK_FALSE(w.log.contains("detenida"));
}

TEST_CASE("telegram: otros mensajes, viejos, grupos y no autorizados", "[telegram][integracion]") {
    World w;
    w.start(config_for({5}), answering({"respuesta"}));
    const std::int64_t old = std::time(nullptr) - 30 * 60; // Hace media hora.
    w.server->push_updates(json::array(
        {chatbot_telegram_test::text_update(1, 5, 5, "", std::time(nullptr)),
         chatbot_telegram_test::text_update(2, 5, 5, "¿me recuerdas?", old),
         chatbot_telegram_test::text_update(3, -100777, 5, "en grupo", std::time(nullptr), "group"),
         chatbot_telegram_test::text_update(4, 9, 9, "hola", std::time(nullptr))}));
    REQUIRE(w.server->wait_for("sendMessage", 3));
    REQUIRE(w.idle());
    const auto mine = w.messages(5);
    REQUIRE(mine.size() == 2);
    CHECK(World::text_of(mine[0]) == chatbot::bot::kTextOnly);
    CHECK(World::text_of(mine[1]) == chatbot::bot::kStaleMessage);
    CHECK(w.messages(-100777).empty());
    const auto stranger = w.messages(9);
    REQUIRE(stranger.size() == 1);
    CHECK(World::text_of(stranger[0]) == chatbot::bot::access_denied_text(9));
    CHECK(w.server->calls("sendRichMessage").empty()); // Nadie llegó al modelo.
}

TEST_CASE("telegram: varios chats con streaming y apagado a la mitad",
          "[telegram][integracion][concurrencia]") {
    World w;
    std::vector<chatbot::bot::UserId> users;
    for (int chat = 1; chat <= 8; ++chat) {
        users.push_back(chat);
    }
    chatbot::bot::BotConfig config = config_for(users);
    config.max_concurrent_responses = 4;
    w.start(config, chatbot_bot_test::slow_clients(100000, 20ms));
    for (int chat = 1; chat <= 8; ++chat) {
        w.say(chat, chat, "hola " + std::to_string(chat));
    }
    // Que haya respuestas a la mitad (con vistas previas).
    for (int i = 0; i < 200 && w.server->calls("sendRichMessageDraft").size() < 8; ++i) {
        std::this_thread::sleep_for(10ms);
    }
    const auto started = std::chrono::steady_clock::now();
    w.stop();
    chatbot_test::report_latency("apagado con 8 chats", std::chrono::steady_clock::now() - started);
    // Nada sale después del apagado.
    const std::size_t before = w.server->calls().size();
    std::this_thread::sleep_for(100ms);
    CHECK(w.server->calls().size() == before);
}

TEST_CASE("telegram: el registro no tiene textos, token ni keys", "[telegram][integracion][registro]") {
    World w;
    const std::string question = "mi pregunta secreta número 42";
    const std::string answer = "la respuesta privada del modelo";
    auto recorder = std::make_shared<chatbot_bot_test::Recorder>();
    w.start(config_for({5}), chatbot_bot_test::scripted_clients(recorder, [&](const auto& messages) {
                // La segunda pregunta falla (también en el reintento con el
                // historial recortado), y el servidor cita el texto.
                if (messages.back().content.find("otra vez") != std::string::npos) {
                    chatbot::HttpResponse error;
                    error.status = 400;
                    error.body = R"({"error":{"message":"no entiendo ')" + messages.back().content +
                                 R"('"}})";
                    return error;
                }
                return chatbot_bot_test::ok_stream({answer});
            }));
    w.say(1, 5, question);
    REQUIRE(w.idle());
    w.say(2, 5, question + " otra vez");
    REQUIRE(w.idle());
    w.say(3, 77, "soy un extraño con /start malcodigo");
    REQUIRE(w.server->wait_for("sendMessage", 2));
    REQUIRE(w.idle());
    w.stop();
    const std::string token{kToken};
    REQUIRE_FALSE(w.log.lines().empty());
    for (const std::string& line : w.log.lines()) {
        INFO(line);
        CHECK(line.find("pregunta secreta") == std::string::npos);
        CHECK(line.find("respuesta privada") == std::string::npos);
        CHECK(line.find("extraño") == std::string::npos);
        CHECK(line.find(token) == std::string::npos);
        CHECK(line.find(token.substr(token.find(':') + 1)) == std::string::npos);
        CHECK(line.find(kModelKey) == std::string::npos);
    }
    CHECK(w.log.contains("error chat=5 usuario=5 origen=modelo"));
    CHECK(w.log.contains("no_autorizado chat=77 usuario=77"));
}
