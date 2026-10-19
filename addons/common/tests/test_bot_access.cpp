#include "bot_harness.hpp"

#include "chatbot/bot/access.h"
#include "chatbot/bot/replies.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

namespace {

using chatbot::bot::AccessControl;
using chatbot::bot::BotConfig;
using chatbot::bot::access_denied_text;
using chatbot::bot::kAccessDenied;
using chatbot::bot::kWelcome;
using chatbot_bot_test::BotHarness;

constexpr const char* kCode = "Codigo_Secreto-1";

} // namespace

TEST_CASE("acceso: AccessControl sin lista ni código no deja entrar a nadie", "[bot][acceso]") {
    AccessControl access{BotConfig{}};
    CHECK_FALSE(access.is_allowed(1));
    CHECK_FALSE(access.redeem(1, ""));
    CHECK_FALSE(access.redeem(1, "cualquier-cosa"));
    CHECK_FALSE(access.is_allowed(1));
}

TEST_CASE("acceso: AccessControl con lista y código", "[bot][acceso]") {
    BotConfig config;
    config.allowed_users = {5};
    config.invite_code = kCode;
    AccessControl access{config};
    CHECK(access.is_allowed(5));
    CHECK_FALSE(access.is_allowed(6));
    CHECK_FALSE(access.redeem(6, "Codigo_Secreto-"));  // Prefijo.
    CHECK_FALSE(access.redeem(6, "Codigo_Secreto-12")); // Más largo.
    CHECK_FALSE(access.redeem(6, "codigo_secreto-1"));  // Distingue mayúsculas.
    CHECK_FALSE(access.redeem(6, ""));
    CHECK(access.invited_count() == 0);
    CHECK(access.redeem(6, kCode));
    CHECK(access.is_allowed(6));
    CHECK(access.invited_count() == 1);
}

TEST_CASE("acceso: sin configuración nadie entra y recibe la frase neutra con su ID",
          "[bot][acceso]") {
    BotHarness h;
    BotConfig config;
    config.system_prompt = "x";
    h.start(config);
    for (const char* text : {"hola", "/start", "/start CODIGO1234", "/ayuda", "/nuevo",
                             "/buscar algo", ""}) {
        h.say(10, 7, text);
    }
    const std::vector<std::string> texts = h.outbox.texts_for(10);
    CHECK(texts == std::vector<std::string>(7, access_denied_text(7)));
    CHECK(access_denied_text(7).rfind(std::string{kAccessDenied}, 0) == 0);
    CHECK(access_denied_text(7).find("Tu ID es 7.") != std::string::npos);
    CHECK(h.recorder->count() == 0);
    CHECK(h.outbox.typing().empty());
    CHECK(h.bot->session_count() == 0);
    // La frase no dice qué modelo, quién administra ni cómo se configura.
    for (const char* hint : {"modelo", "administrador", "config", "código", "/start"}) {
        CHECK(access_denied_text(7).find(hint) == std::string::npos);
    }
    // Su ID queda en el registro una sola vez, para que lo agreguen.
    std::size_t reported = 0;
    for (const std::string& line : h.log.lines()) {
        if (line.find("no_autorizado chat=10 usuario=7") != std::string::npos) {
            ++reported;
        }
    }
    CHECK(reported == 1);
}

TEST_CASE("acceso: un ID de la lista entra", "[bot][acceso]") {
    BotHarness h;
    BotConfig config = chatbot_bot_test::test_bot_config();
    config.allowed_users = {42};
    h.start(config);
    h.say(10, 42, "hola");
    CHECK(h.outbox.texts_for(10) == std::vector<std::string>{"eco: hola"});
    h.say(11, 43, "hola");
    CHECK(h.outbox.texts_for(11) == std::vector<std::string>{access_denied_text(43)});
}

TEST_CASE("acceso: código correcto, incorrecto y vacío", "[bot][acceso]") {
    BotHarness h;
    BotConfig config = chatbot_bot_test::test_bot_config();
    config.allowed_users.clear();
    config.invite_code = kCode;
    h.start(config);

    h.say(10, 7, "/start malcodigo");
    h.say(10, 7, "/start");
    h.say(10, 7, "hola");
    CHECK(h.outbox.texts_for(10) == std::vector<std::string>(3, access_denied_text(7)));
    CHECK(h.recorder->count() == 0);

    h.outbox.clear();
    h.say(10, 7, std::string{"/start "} + kCode);
    h.say(10, 7, "hola");
    CHECK(h.outbox.texts_for(10) == std::vector<std::string>{std::string{kWelcome}, "eco: hola"});
    // /start con el código otra vez, ya autorizado: solo la bienvenida.
    h.say(10, 7, std::string{"/start "} + kCode);
    CHECK(h.outbox.texts_for(10).back() == kWelcome);

    // Otro usuario en el mismo chat (un grupo) sigue sin acceso.
    h.say(10, 8, "hola");
    CHECK(h.outbox.texts_for(10).back() == access_denied_text(8));

    CHECK(h.log.contains("invitacion_rechazada chat=10 usuario=7"));
    CHECK(h.log.contains("invitacion_aceptada chat=10 usuario=7"));
    CHECK_FALSE(h.log.contains(kCode));
}

TEST_CASE("acceso: un extraño que insiste no hace crecer el registro", "[bot][acceso]") {
    BotHarness h;
    BotConfig config = chatbot_bot_test::test_bot_config();
    config.invite_code = kCode;
    h.start(config);
    for (int i = 0; i < 20; ++i) {
        h.say(10, 7, "hola " + std::to_string(i));
        h.say(10, 7, "/start intento" + std::to_string(i) + "abc");
    }
    std::size_t reported = 0;
    std::size_t rejected = 0;
    for (const std::string& line : h.log.lines()) {
        reported += line.find("no_autorizado chat=10 usuario=7") != std::string::npos ? 1 : 0;
        rejected += line.find("invitacion_rechazada chat=10 usuario=7") != std::string::npos ? 1 : 0;
        // Sus mensajes no se anotan uno por uno.
        CHECK(line.find("mensaje plataforma=prueba chat=10 usuario=7") == std::string::npos);
    }
    CHECK(reported == 1);
    CHECK(rejected == 3);
    CHECK(h.outbox.texts_for(10).size() == 40); // Pero cada uno recibe la frase neutra.
}
