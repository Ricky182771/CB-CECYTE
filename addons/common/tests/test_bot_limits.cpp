#include "bot_harness.hpp"

#include "chatbot/bot/replies.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using chatbot::bot::kWaitPrevious;
using chatbot_bot_test::BotHarness;
using chatbot_bot_test::Gate;
using chatbot_bot_test::gated_clients;
using namespace std::chrono_literals;

} // namespace

TEST_CASE("límites: un mensaje durante una respuesta recibe un aviso y no se encola",
          "[bot][limites][hilos]") {
    BotHarness h;
    const auto gate = std::make_shared<Gate>();
    h.start(chatbot_bot_test::test_bot_config(), gated_clients(gate, h.recorder));
    h.bot->handle(chatbot_bot_test::message(10, 1, "uno"));
    REQUIRE(gate->wait_inside(1));
    h.bot->handle(chatbot_bot_test::message(10, 1, "dos"));
    h.bot->handle(chatbot_bot_test::message(10, 1, "/nuevo"));
    CHECK(h.outbox.texts_for(10) ==
          std::vector<std::string>{std::string{kWaitPrevious}, std::string{kWaitPrevious}});
    // /ayuda y /start no esperan.
    h.bot->handle(chatbot_bot_test::message(10, 1, "/ayuda"));
    CHECK(h.outbox.texts_for(10).size() == 3);

    gate->open();
    REQUIRE(h.bot->wait_idle(chatbot_test::kThreadTimeout));
    CHECK(h.outbox.texts_for(10).back() == "eco: uno");
    CHECK(h.recorder->count() == 1); // "dos" nunca llegó al modelo.
    // Ya libre, el chat vuelve a aceptar mensajes.
    h.say(10, 1, "tres");
    CHECK(h.outbox.texts_for(10).back() == "eco: tres");
}

TEST_CASE("límites: tope diario por usuario, también al cambiar de día", "[bot][limites]") {
    BotHarness h;
    auto config = chatbot_bot_test::test_bot_config();
    config.allowed_users = {1, 2};
    config.daily_message_limit = 2;
    h.start(config);
    h.say(10, 1, "a");
    h.say(10, 1, "b");
    h.say(10, 1, "c");
    CHECK(h.outbox.texts_for(10) ==
          std::vector<std::string>{"eco: a", "eco: b", chatbot::bot::daily_limit_reached(2)});
    CHECK(h.recorder->count() == 2);
    CHECK(h.log.contains("tope_diario chat=10 usuario=1"));
    // Los comandos que no llaman al modelo no cuentan ni se bloquean.
    h.say(10, 1, "/ayuda");
    h.say(10, 1, "/nuevo");
    CHECK(h.outbox.texts_for(10).back() == chatbot::bot::kNewConversation);
    // Otro usuario tiene su propio tope.
    h.say(20, 2, "x");
    CHECK(h.outbox.texts_for(20) == std::vector<std::string>{"eco: x"});

    // Unas horas después sigue siendo el mismo día (el reloj empieza a mediodía).
    h.clock.advance(6h);
    h.say(10, 1, "d");
    CHECK(h.outbox.texts_for(10).back() == chatbot::bot::daily_limit_reached(2));
    // Al día siguiente vuelve a poder.
    h.clock.advance(24h);
    h.say(10, 1, "e");
    CHECK(h.outbox.texts_for(10).back() == "eco: e");
}

TEST_CASE("límites: tope global de respuestas simultáneas, en orden de llegada",
          "[bot][limites][hilos]") {
    BotHarness h;
    auto config = chatbot_bot_test::test_bot_config();
    config.allowed_users = {1, 2, 3, 4, 5, 6};
    config.max_concurrent_responses = 2;
    const auto gate = std::make_shared<Gate>();
    h.start(config, gated_clients(gate, h.recorder));
    for (int i = 1; i <= 6; ++i) {
        h.bot->handle(chatbot_bot_test::message(i * 10, i, "pregunta " + std::to_string(i)));
    }
    REQUIRE(gate->wait_inside(2));
    // Las demás esperan en la fila: no llegan al modelo aunque pase el tiempo.
    std::this_thread::sleep_for(100ms);
    CHECK(gate->inside() == 2);
    CHECK(h.recorder->count() == 2);
    CHECK(h.outbox.sent().empty()); // Nadie recibe un aviso: esperan su turno.

    gate->open();
    REQUIRE(h.bot->wait_idle(chatbot_test::kThreadTimeout));
    CHECK(gate->max_inside() == 2);
    for (int i = 1; i <= 6; ++i) {
        CHECK(h.outbox.texts_for(i * 10) ==
              std::vector<std::string>{"eco: pregunta " + std::to_string(i)});
    }
}

TEST_CASE("límites: la fila respeta el orden de llegada", "[bot][limites][hilos]") {
    BotHarness h;
    auto config = chatbot_bot_test::test_bot_config();
    config.allowed_users = {1, 2, 3, 4, 5, 6};
    config.max_concurrent_responses = 1;
    const auto gate = std::make_shared<Gate>();
    h.start(config, gated_clients(gate, h.recorder));
    for (int i = 1; i <= 6; ++i) {
        h.bot->handle(chatbot_bot_test::message(i * 10, i, "pregunta " + std::to_string(i)));
    }
    REQUIRE(gate->wait_inside(1));
    gate->open();
    REQUIRE(h.bot->wait_idle(chatbot_test::kThreadTimeout));
    std::vector<std::string> expected;
    for (int i = 1; i <= 6; ++i) {
        expected.push_back("pregunta " + std::to_string(i));
    }
    CHECK(gate->order() == expected);
}
