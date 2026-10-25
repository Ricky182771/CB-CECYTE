// Zona horaria del tope diario (day_in_zone), invitados guardados y el
// detalle de los errores en el registro.

#include "bot_harness.hpp"

#include "chatbot/bot/replies.h"
#include "chatbot/addon_settings/time_zone.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace {

using chatbot::addon_settings::day_in_zone;
using chatbot::addon_settings::DayFunction;
using chatbot_bot_test::BotHarness;
using namespace std::chrono;

/// Instante en UTC.
system_clock::time_point utc(int y, unsigned m, unsigned d, int hour, int minute) {
    return sys_days{year{y} / month{m} / day{d}} + hours{hour} + minutes{minute};
}

DayFunction zone(std::string_view name) {
    auto result = day_in_zone(name);
    REQUIRE(result.is_ok());
    return result.value();
}

} // namespace

TEST_CASE("bot: el tope diario cambia a medianoche local, no a medianoche UTC", "[bot][zona]") {
    BotHarness h;
    h.day = zone(chatbot::addon_settings::default_time_zone()); // America/Mexico_City o -06:00.
    chatbot::bot::BotConfig config = chatbot_bot_test::test_bot_config();
    config.daily_message_limit = 1;
    config.session_idle_timeout = minutes{24 * 60};
    h.start(config);

    h.clock.set(utc(2026, 10, 17, 23, 30)); // 17:30 del 17 en México.
    h.say(10, 1, "uno");
    CHECK(h.outbox.texts_for(10).back() == "eco: uno");
    h.clock.set(utc(2026, 10, 18, 0, 30)); // Ya es 18 en UTC, pero 18:30 del 17 aquí.
    h.say(10, 1, "dos");
    CHECK(h.outbox.texts_for(10).back() == chatbot::bot::daily_limit_reached(1));
    h.clock.set(utc(2026, 10, 18, 6, 30)); // 00:30 del 18 en México.
    h.say(10, 1, "tres");
    CHECK(h.outbox.texts_for(10).back() == "eco: tres");
}

TEST_CASE("bot: los invitados por código se guardan y se leen", "[bot][acceso]") {
    BotHarness h;
    // Un invitados.json de mentira: add_invited devuelve lo que quedó.
    std::vector<chatbot::bot::UserId> file{50};
    std::vector<chatbot::bot::UserId> added;
    std::mutex file_mutex;
    h.invited = {50};
    h.add_invited = [&](chatbot::bot::UserId user) {
        const std::lock_guard<std::mutex> lock(file_mutex);
        added.push_back(user);
        file.push_back(user);
        return std::optional<std::vector<chatbot::bot::UserId>>{file};
    };
    chatbot::bot::BotConfig config = chatbot_bot_test::test_bot_config();
    config.allowed_users.clear();
    config.invite_code = "codigo_secreto_1";
    h.start(config);

    // El de antes entra sin código.
    h.say(20, 50, "hola");
    CHECK(h.outbox.texts_for(20).back() == "eco: hola");
    h.say(21, 51, "/start codigo_secreto_1");
    h.say(22, 52, "/start codigo_secreto_1");
    h.say(23, 53, "/start otro_codigo_mal");
    const std::lock_guard<std::mutex> lock(file_mutex);
    CHECK(added == std::vector<chatbot::bot::UserId>{51, 52});
    CHECK(file == std::vector<chatbot::bot::UserId>{50, 51, 52});
}

TEST_CASE("bot: la lista que devuelve el archivo reemplaza a la de memoria", "[bot][acceso]") {
    BotHarness h;
    // Otro proceso (la app) quitó al 50 del archivo: al guardar al 51, el
    // bot se queda con la lista del archivo y el 50 ya no entra.
    h.invited = {50};
    h.add_invited = [](chatbot::bot::UserId user) {
        return std::optional<std::vector<chatbot::bot::UserId>>{{user}};
    };
    chatbot::bot::BotConfig config = chatbot_bot_test::test_bot_config();
    config.allowed_users.clear();
    config.invite_code = "codigo_secreto_1";
    h.start(config);
    h.say(20, 50, "hola");
    CHECK(h.outbox.texts_for(20).back() == "eco: hola");
    h.say(21, 51, "/start codigo_secreto_1");
    h.say(20, 50, "otra vez");
    CHECK(h.outbox.texts_for(20).back() == chatbot::bot::access_denied_text(50));
    h.say(21, 51, "hola");
    CHECK(h.outbox.texts_for(21).back() == "eco: hola");
}

TEST_CASE("bot: si no se pueden guardar los invitados, entran igual y se anota", "[bot][acceso]") {
    BotHarness h;
    h.add_invited = [](chatbot::bot::UserId) {
        return std::optional<std::vector<chatbot::bot::UserId>>{};
    };
    chatbot::bot::BotConfig config = chatbot_bot_test::test_bot_config();
    config.invite_code = "codigo_secreto_1";
    h.start(config);
    h.say(21, 51, "/start codigo_secreto_1");
    h.say(21, 51, "hola");
    CHECK(h.outbox.texts_for(21).back() == "eco: hola");
    CHECK(h.log.contains("invitados_no_guardados n=1"));
}

TEST_CASE("bot: el detalle del error solo va al registro con log_error_details", "[bot]") {
    for (const bool details : {false, true}) {
        BotHarness h;
        chatbot::bot::BotConfig config = chatbot_bot_test::test_bot_config();
        config.log_error_details = details;
        h.start(config, chatbot_bot_test::scripted_clients(h.recorder, [](const auto&) {
                    chatbot::HttpResponse response;
                    response.status = 400;
                    response.body = R"({"error":{"message":"no entiendo 'texto del alumno'"}})";
                    return response;
                }));
        h.say(10, 1, "texto del alumno");
        CHECK(h.log.contains("error chat=10 usuario=1 origen=modelo"));
        CHECK(h.log.contains("http=400"));
        CHECK(h.log.contains("detalle=") == details);
        CHECK(h.log.contains("texto del alumno") == details);
    }
}
