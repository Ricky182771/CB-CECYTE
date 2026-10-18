#include "bot_harness.hpp"

#include "chatbot/bot/replies.h"
#include "chatbot/bot/sessions.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace {

using chatbot::Message;
using chatbot::Role;
using chatbot::bot::Session;
using chatbot::bot::SessionStore;
using chatbot_bot_test::BotHarness;
using chatbot_test::check_invariants;
using chatbot_test::Ending;
using chatbot_test::sent_messages_of;
using namespace std::chrono_literals;

using Pairs = std::vector<std::pair<Role, std::string>>;

/// Roles y textos de la última petición al modelo (Message no tiene ==).
Pairs last_request(const BotHarness& h) {
    const auto requests = h.recorder->requests();
    REQUIRE_FALSE(requests.empty());
    Pairs out;
    for (const Message& message : sent_messages_of(requests.back())) {
        out.emplace_back(message.role, message.content);
    }
    return out;
}

} // namespace

TEST_CASE("sesiones: Session solo crece de par en par", "[bot][sesiones]") {
    const auto now = std::chrono::system_clock::now();
    Session session{"sistema", now};
    CHECK(session.history().size() == 1);
    const std::vector<Message> request = session.request("hola");
    CHECK(check_invariants(request, "sistema", Ending::Sent, "hola").empty());
    CHECK(session.history().size() == 1); // request no cambia el historial.
    session.commit("hola", "qué tal");
    CHECK(check_invariants(session.history(), "sistema", Ending::Idle).empty());
    CHECK(session.history().size() == 3);

    const Session empty{"", now};
    CHECK(empty.history().empty());
}

TEST_CASE("sesiones: SessionStore expira, borra y respeta las ocupadas", "[bot][sesiones]") {
    const auto start = std::chrono::system_clock::now();
    SessionStore store{"sistema", std::chrono::minutes{60}};
    Session& first = store.get(10, start);
    first.commit("a", "b");
    CHECK(store.get(10, start + 59min).history().size() == 3);
    // Al expirar, get() da una nueva.
    CHECK(store.get(10, start + 60min).history().size() == 1);

    store.get(20, start).busy = true;
    store.get(30, start);
    CHECK(store.expire(start + 2h) == 2); // La 10 y la 30; la 20 está ocupada.
    CHECK(store.size() == 1);
    CHECK_FALSE(store.reset(20));
    store.find(20)->busy = false;
    CHECK(store.reset(20));
    CHECK(store.size() == 0);
    CHECK(store.reset(99)); // Sin sesión: nada que borrar.
}

TEST_CASE("sesiones: dos chats no se mezclan", "[bot][sesiones]") {
    BotHarness h;
    auto config = chatbot_bot_test::test_bot_config();
    config.allowed_users = {1, 2};
    h.start(config);
    h.say(10, 1, "uno");
    h.say(20, 2, "dos");
    h.say(10, 1, "tres");
    CHECK(last_request(h) == Pairs{{Role::System, "Eres un bot de prueba."},
                                                  {Role::User, "uno"},
                                                  {Role::Assistant, "eco: uno"},
                                                  {Role::User, "tres"}});
    h.say(20, 2, "cuatro");
    CHECK(last_request(h) == Pairs{{Role::System, "Eres un bot de prueba."},
                                                  {Role::User, "dos"},
                                                  {Role::Assistant, "eco: dos"},
                                                  {Role::User, "cuatro"}});
    CHECK(h.outbox.texts_for(10) == std::vector<std::string>{"eco: uno", "eco: tres"});
}

TEST_CASE("sesiones: se borran por inactividad", "[bot][sesiones]") {
    BotHarness h;
    h.start();
    h.say(10, 1, "uno");
    h.clock.advance(59min);
    h.say(10, 1, "dos");
    CHECK(last_request(h).size() == 4); // Sigue: la actividad reinicia el tiempo.
    h.clock.advance(60min);
    h.say(10, 1, "tres");
    CHECK(last_request(h) == Pairs{{Role::System, "Eres un bot de prueba."},
                                                  {Role::User, "tres"}});
    // Sin mensajes nuevos, expire_idle() la borra.
    CHECK(h.bot->session_count() == 1);
    h.clock.advance(61min);
    h.bot->expire_idle();
    CHECK(h.bot->session_count() == 0);
    CHECK(h.log.contains("sesiones_borradas n=1"));
}

TEST_CASE("sesiones: /nuevo empieza otra conversación", "[bot][sesiones]") {
    BotHarness h;
    h.start();
    h.say(10, 1, "uno");
    h.say(10, 1, "/nuevo");
    CHECK(h.outbox.texts_for(10).back() == chatbot::bot::kNewConversation);
    CHECK(h.bot->session_count() == 0);
    h.say(10, 1, "dos");
    CHECK(last_request(h) == Pairs{{Role::System, "Eres un bot de prueba."},
                                                  {Role::User, "dos"}});
}

TEST_CASE("sesiones: el system prompt nunca se pierde ni se duplica", "[bot][sesiones]") {
    // Operaciones al azar en tres chats: mensajes que salen bien o fallan,
    // respuestas vacías, /nuevo y el reloj que avanza. Cada petición que
    // llega al modelo cumple los invariantes del PR #22.
    for (const std::string prompt : {"Eres un bot de prueba.", ""}) {
        BotHarness h;
        auto config = chatbot_bot_test::test_bot_config();
        config.allowed_users = {1, 2, 3};
        config.system_prompt = prompt;
        config.daily_message_limit = chatbot::bot::kMaxDailyMessages;
        h.start(config, chatbot_bot_test::scripted_clients(
                            h.recorder, [](const std::vector<Message>& messages) {
                                const std::string& last = messages.back().content;
                                chatbot::HttpResponse response;
                                if (last.rfind("falla", 0) == 0) {
                                    response.status = 400;
                                    response.body = R"({"error":{"message":"no"}})";
                                    return response;
                                }
                                if (last.rfind("vacía", 0) == 0) {
                                    return chatbot_bot_test::ok_stream({"  "});
                                }
                                return chatbot_bot_test::echo(messages);
                            }));
        std::mt19937 random{22};
        const std::vector<std::string> kinds = {"hola", "falla", "vacía", "/nuevo", "reloj"};
        std::size_t checked = 0;
        for (int step = 0; step < 300; ++step) {
            const auto chat = static_cast<chatbot::bot::ChatId>(10 + random() % 3);
            const auto user = static_cast<chatbot::bot::UserId>(chat - 9);
            const std::string kind = kinds[random() % kinds.size()];
            if (kind == "reloj") {
                h.clock.advance(std::chrono::minutes{random() % 90});
                continue;
            }
            const std::string text = kind == "/nuevo" ? kind : kind + " " + std::to_string(step);
            const std::size_t before = h.recorder->count();
            h.say(chat, user, text);
            const auto requests = h.recorder->requests();
            for (std::size_t i = before; i < requests.size(); ++i) {
                INFO("paso " << step << " (" << text << "), petición " << i);
                REQUIRE(check_invariants(sent_messages_of(requests[i]), prompt, Ending::Sent,
                                         text)
                            .empty());
                ++checked;
            }
        }
        CHECK(checked > 100);
    }
}
