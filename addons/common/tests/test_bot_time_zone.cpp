// Zona horaria del tope diario (day_in_zone), invitados guardados y el
// detalle de los errores en el registro.

#include "bot_harness.hpp"

#include "chatbot/bot/replies.h"
#include "chatbot/bot/time_zone.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>
#include <vector>

namespace {

using chatbot::bot::day_at_offset;
using chatbot::bot::day_in_zone;
using chatbot::bot::DayFunction;
using chatbot::bot::parse_utc_offset;
using chatbot::bot::time_zone_database_available;
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

TEST_CASE("zona horaria: el día cambia a medianoche de la Ciudad de México", "[bot][zona]") {
    if (!time_zone_database_available()) {
        SKIP("esta compilación no tiene la base de zonas de C++20");
    }
    const DayFunction mexico = zone("America/Mexico_City");
    // UTC−6 todo el año (sin horario de verano desde 2022).
    CHECK(mexico(utc(2026, 10, 18, 0, 0)) == "2026-10-17");  // Medianoche UTC.
    CHECK(mexico(utc(2026, 10, 18, 5, 59)) == "2026-10-17");
    CHECK(mexico(utc(2026, 10, 18, 6, 0)) == "2026-10-18");
    CHECK(mexico(utc(2026, 1, 1, 5, 0)) == "2025-12-31");
}

TEST_CASE("zona horaria: con horario de verano", "[bot][zona]") {
    if (!time_zone_database_available()) {
        SKIP("esta compilación no tiene la base de zonas de C++20");
    }
    const DayFunction new_york = zone("America/New_York");
    // Verano: UTC−4.
    CHECK(new_york(utc(2026, 7, 1, 3, 59)) == "2026-06-30");
    CHECK(new_york(utc(2026, 7, 1, 4, 0)) == "2026-07-01");
    // Después del 1 de noviembre de 2026: UTC−5.
    CHECK(new_york(utc(2026, 11, 2, 4, 30)) == "2026-11-01");
    CHECK(new_york(utc(2026, 11, 2, 5, 0)) == "2026-11-02");
}

TEST_CASE("zona horaria: un desfase fijo funciona siempre", "[bot][zona]") {
    // Lo mismo que la Ciudad de México, sin base de zonas.
    const DayFunction minus_six = zone("-06:00");
    CHECK(minus_six(utc(2026, 10, 18, 0, 0)) == "2026-10-17");
    CHECK(minus_six(utc(2026, 10, 18, 5, 59)) == "2026-10-17");
    CHECK(minus_six(utc(2026, 10, 18, 6, 0)) == "2026-10-18");
    CHECK(minus_six(utc(2026, 1, 1, 5, 0)) == "2025-12-31");
    const DayFunction india = zone("+05:30");
    CHECK(india(utc(2026, 10, 17, 18, 29)) == "2026-10-17");
    CHECK(india(utc(2026, 10, 17, 18, 30)) == "2026-10-18");
    CHECK(zone("+00:00")(utc(2026, 10, 17, 23, 59)) == "2026-10-17");
    CHECK(zone("+14:00")(utc(2026, 10, 17, 10, 0)) == "2026-10-18");
    CHECK(zone("-12:00")(utc(2026, 10, 17, 11, 59)) == "2026-10-16");
    // Antes de 1970 también.
    CHECK(day_at_offset(minutes{-360})(utc(1969, 12, 31, 3, 0)) == "1969-12-30");

    REQUIRE(parse_utc_offset("-06:00").is_ok());
    CHECK(parse_utc_offset("-06:00").value() == minutes{-360});
    CHECK(parse_utc_offset("+05:45").value() == minutes{345});
    for (const char* bad : {"-6:00", "-0600", "06:00", "+06:0", "-06:00 ", "+15:00", "-12:15",
                            "+05:20", "+aa:bb", "−06:00"}) {
        INFO(bad);
        const auto result = parse_utc_offset(bad);
        REQUIRE(result.is_error());
        CHECK(result.error().kind == chatbot::ErrorKind::Config);
    }
    const auto bad_zone = day_in_zone("-6");
    REQUIRE(bad_zone.is_error());
    CHECK(bad_zone.error().message.find("Zona horaria") != std::string::npos);
    CHECK(bad_zone.error().message.find("-HH:MM") != std::string::npos);
}

TEST_CASE("zona horaria: el valor por defecto funciona en esta compilación", "[bot][zona]") {
    const std::string_view name = chatbot::bot::default_time_zone();
    CHECK(name == (time_zone_database_available() ? chatbot::bot::kDefaultTimeZone
                                                  : chatbot::bot::kDefaultUtcOffset));
    const DayFunction day = zone(name);
    CHECK(day(utc(2026, 10, 18, 5, 59)) == "2026-10-17");
    CHECK(day(utc(2026, 10, 18, 6, 0)) == "2026-10-18");
}

TEST_CASE("zona horaria: sin base de zonas, un nombre pide un desfase", "[bot][zona]") {
    if (time_zone_database_available()) {
        SKIP("esta compilación tiene la base de zonas de C++20");
    }
    const auto result = day_in_zone("America/Mexico_City");
    REQUIRE(result.is_error());
    CHECK(result.error().kind == chatbot::ErrorKind::Config);
    CHECK(result.error().message.find("base de zonas") != std::string::npos);
    CHECK(result.error().message.find("-06:00") != std::string::npos);
    // No sugiere otro nombre, que tampoco serviría.
    CHECK(result.error().message.find("Usa un nombre") == std::string::npos);
}

TEST_CASE("zona horaria: un nombre inválido es un error claro", "[bot][zona]") {
    for (const char* name : {"", "Marte/Base", "America/Ciudad_de_Mexico", "UTC+6"}) {
        const auto result = day_in_zone(name);
        REQUIRE(result.is_error());
        CHECK(result.error().kind == chatbot::ErrorKind::Config);
        CHECK(result.error().message.find("Zona horaria") != std::string::npos);
        CHECK(result.error().message.find("-06:00") != std::string::npos);
    }
}

TEST_CASE("bot: el tope diario cambia a medianoche local, no a medianoche UTC", "[bot][zona]") {
    BotHarness h;
    h.day = zone(chatbot::bot::default_time_zone()); // America/Mexico_City o -06:00.
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
    std::vector<std::vector<chatbot::bot::UserId>> saved;
    std::mutex saved_mutex;
    h.invited = {50};
    h.save_invited = [&](const std::vector<chatbot::bot::UserId>& users) {
        const std::lock_guard<std::mutex> lock(saved_mutex);
        saved.push_back(users);
        return true;
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
    const std::lock_guard<std::mutex> lock(saved_mutex);
    REQUIRE(saved.size() == 2);
    CHECK(saved[0] == std::vector<chatbot::bot::UserId>{50, 51});
    CHECK(saved[1] == std::vector<chatbot::bot::UserId>{50, 51, 52});
}

TEST_CASE("bot: si no se pueden guardar los invitados, entran igual y se anota", "[bot][acceso]") {
    BotHarness h;
    h.save_invited = [](const std::vector<chatbot::bot::UserId>&) { return false; };
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
