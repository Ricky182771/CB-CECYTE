// Bot::reconfigure: la recarga en caliente (SIGHUP en Telegram). Las
// conversaciones se conservan, las respuestas en curso terminan con la
// configuración de antes y quien pierde el acceso pierde sus conversaciones.

#include "bot_harness.hpp"

#include "chatbot/bot/replies.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace {

using chatbot::Message;
using chatbot::Role;
using chatbot::bot::BotConfig;
using chatbot_bot_test::BotHarness;
using chatbot_bot_test::Gate;
using chatbot_bot_test::Recorder;
using chatbot_test::sent_messages_of;

/// Recarga con config y las fábricas dadas (sin búsqueda ni día).
void reload(BotHarness& h, BotConfig config, chatbot::bot::ClientFactory clients,
            std::vector<chatbot::bot::UserId> invited = {},
            chatbot::addon_settings::DayFunction day = {}) {
    h.bot->reconfigure(chatbot_bot_test::valid(std::move(config)), std::move(clients), {},
                       std::move(day), std::move(invited));
}

/// Contenidos de la última petición que anotó recorder.
std::vector<std::string> last_contents(const Recorder& recorder) {
    const auto requests = recorder.requests();
    REQUIRE_FALSE(requests.empty());
    std::vector<std::string> out;
    for (const Message& message : sent_messages_of(requests.back())) {
        out.push_back(message.content);
    }
    return out;
}

} // namespace

TEST_CASE("recarga: aplica en caliente y conserva las conversaciones", "[bot][recarga]") {
    BotHarness h;
    h.start();
    h.say(10, 1, "uno");

    BotConfig config = chatbot_bot_test::test_bot_config();
    config.allowed_users = {1, 2};
    config.daily_message_limit = 2;
    config.system_prompt = "Instrucciones nuevas.";
    reload(h, config, chatbot_bot_test::scripted_clients(h.recorder));
    CHECK(h.bot->session_count() == 1);

    // La conversación sigue, con sus instrucciones de antes.
    h.say(10, 1, "dos");
    CHECK(last_contents(*h.recorder) ==
          std::vector<std::string>{"Eres un bot de prueba.", "uno", "eco: uno", "dos"});
    // El tope nuevo cuenta los mensajes que ya se mandaron hoy.
    h.say(10, 1, "tres");
    CHECK(h.outbox.texts_for(10).back() == chatbot::bot::daily_limit_reached(2));
    // Un chat nuevo empieza con las instrucciones nuevas; el 2 ya entra.
    h.say(20, 2, "hola");
    CHECK(last_contents(*h.recorder) == std::vector<std::string>{"Instrucciones nuevas.", "hola"});
    CHECK_FALSE(h.log.contains("requiere_reinicio"));
}

TEST_CASE("recarga: una respuesta en curso termina con la configuración de antes",
          "[bot][recarga][hilos]") {
    BotHarness h;
    const auto gate = std::make_shared<Gate>();
    const auto old_recorder = std::make_shared<Recorder>();
    h.start(chatbot_bot_test::test_bot_config(),
            chatbot_bot_test::gated_clients(gate, old_recorder));
    h.bot->handle(chatbot_bot_test::message(10, 1, "uno"));
    REQUIRE(gate->wait_inside(1));

    BotConfig config = chatbot_bot_test::test_bot_config();
    config.system_prompt = "Instrucciones nuevas.";
    reload(h, config, chatbot_bot_test::scripted_clients(h.recorder));
    gate->open();
    REQUIRE(h.bot->wait_idle(chatbot_test::kThreadTimeout));
    CHECK(h.outbox.texts_for(10) == std::vector<std::string>{"eco: uno"});
    CHECK(old_recorder->count() == 1);
    CHECK(h.recorder->count() == 0);

    // La siguiente usa el cliente nuevo y conserva el par de antes.
    h.say(10, 1, "dos");
    CHECK(last_contents(*h.recorder) ==
          std::vector<std::string>{"Eres un bot de prueba.", "uno", "eco: uno", "dos"});
}

TEST_CASE("recarga: quien pierde el acceso pierde sus conversaciones", "[bot][recarga][hilos]") {
    BotHarness h;
    const auto gate = std::make_shared<Gate>();
    BotConfig config = chatbot_bot_test::test_bot_config();
    config.allowed_users = {1, 2, 3};
    config.max_concurrent_responses = 1;
    h.start(config, chatbot_bot_test::gated_clients(gate, h.recorder));
    gate->open();
    h.say(30, 3, "antes");  // Sesión libre del 3.
    h.say(10, 1, "antes");  // Sesión libre del 1, que sigue autorizado.

    const auto closed = std::make_shared<Gate>();
    const auto closed_recorder = std::make_shared<Recorder>();
    reload(h, config, chatbot_bot_test::gated_clients(closed, closed_recorder));
    h.bot->handle(chatbot_bot_test::message(20, 2, "en curso"));
    REQUIRE(closed->wait_inside(1));
    h.bot->handle(chatbot_bot_test::message(30, 3, "en la fila"));

    config.allowed_users = {1};
    reload(h, config, chatbot_bot_test::gated_clients(closed, closed_recorder));
    CHECK(h.log.contains("acceso_retirado sesiones=1 descartadas=1"));
    closed->open();
    REQUIRE(h.bot->wait_idle(chatbot_test::kThreadTimeout));
    // La respuesta en curso salió; la de la fila nunca llegó al modelo.
    CHECK(h.outbox.texts_for(20) == std::vector<std::string>{"eco: en curso"});
    CHECK(closed_recorder->count() == 1);
    CHECK(h.bot->session_count() == 1); // Solo la del 1.

    // Ya no entran; si vuelven a tener acceso, empiezan de cero.
    h.say(20, 2, "otra");
    CHECK(h.outbox.texts_for(20).back() == chatbot::bot::access_denied_text(2));
    config.allowed_users = {1, 2};
    reload(h, config, chatbot_bot_test::gated_clients(closed, closed_recorder));
    h.say(20, 2, "de nuevo");
    CHECK(last_contents(*closed_recorder) ==
          std::vector<std::string>{"Eres un bot de prueba.", "de nuevo"});
}

TEST_CASE("recarga: los invitados salen de la lista guardada", "[bot][recarga]") {
    BotHarness h;
    h.invited = {5};
    h.start();
    h.say(50, 5, "hola");
    CHECK(h.outbox.texts_for(50) == std::vector<std::string>{"eco: hola"});
    reload(h, chatbot_bot_test::test_bot_config(), chatbot_bot_test::scripted_clients(h.recorder),
           {6});
    h.say(50, 5, "sigo");
    CHECK(h.outbox.texts_for(50).back() == chatbot::bot::access_denied_text(5));
    h.say(60, 6, "entro");
    CHECK(h.outbox.texts_for(60) == std::vector<std::string>{"eco: entro"});
    CHECK(h.bot->session_count() == 1);
}

TEST_CASE("recarga: otra zona horaria con otra fecha empieza otro día", "[bot][recarga]") {
    BotHarness h;
    h.day = [](auto) { return std::string{"2026-10-24"}; };
    BotConfig config = chatbot_bot_test::test_bot_config();
    config.daily_message_limit = 1;
    h.start(config);
    h.say(10, 1, "uno");
    h.say(10, 1, "dos");
    CHECK(h.outbox.texts_for(10).back() == chatbot::bot::daily_limit_reached(1));
    reload(h, config, chatbot_bot_test::scripted_clients(h.recorder), {},
           [](auto) { return std::string{"2026-10-25"}; });
    h.say(10, 1, "tres");
    CHECK(h.outbox.texts_for(10).back() == "eco: tres");
}

TEST_CASE("recarga: max_concurrent_responses necesita reiniciar", "[bot][recarga]") {
    BotHarness h;
    h.start();
    BotConfig config = chatbot_bot_test::test_bot_config();
    config.max_concurrent_responses = 8;
    reload(h, config, chatbot_bot_test::scripted_clients(h.recorder));
    CHECK(h.log.contains("requiere_reinicio campo=max_concurrent_responses"));
    h.bot->shutdown();
    reload(h, config, chatbot_bot_test::scripted_clients(h.recorder)); // No hace nada.
}

TEST_CASE("recarga: la inactividad nueva vale para las conversaciones que ya existen",
          "[bot][recarga]") {
    using namespace std::chrono_literals;
    BotHarness h;
    h.start();
    h.say(10, 1, "uno");
    BotConfig config = chatbot_bot_test::test_bot_config();
    config.session_idle_timeout = 5min;
    reload(h, config, chatbot_bot_test::scripted_clients(h.recorder));
    h.clock.advance(6min);
    h.bot->expire_idle();
    CHECK(h.bot->session_count() == 0);
}
