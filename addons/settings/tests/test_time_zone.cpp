// Zona horaria del tope diario: day_in_zone, parse_utc_offset y el valor por
// defecto (chatbot::addon_settings).

#include "chatbot/addon_settings/time_zone.h"

#include "chatbot/error.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>

namespace {

using chatbot::addon_settings::day_at_offset;
using chatbot::addon_settings::day_in_zone;
using chatbot::addon_settings::DayFunction;
using chatbot::addon_settings::parse_utc_offset;
using chatbot::addon_settings::time_zone_database_available;
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

TEST_CASE("zona horaria: el día cambia a medianoche de la Ciudad de México",
          "[addon_settings][zona]") {
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

TEST_CASE("zona horaria: con horario de verano", "[addon_settings][zona]") {
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

TEST_CASE("zona horaria: un desfase fijo funciona siempre", "[addon_settings][zona]") {
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

TEST_CASE("zona horaria: el valor por defecto funciona en esta compilación", "[addon_settings][zona]") {
    const std::string_view name = chatbot::addon_settings::default_time_zone();
    CHECK(name == (time_zone_database_available() ? chatbot::addon_settings::kDefaultTimeZone
                                                  : chatbot::addon_settings::kDefaultUtcOffset));
    const DayFunction day = zone(name);
    CHECK(day(utc(2026, 10, 18, 5, 59)) == "2026-10-17");
    CHECK(day(utc(2026, 10, 18, 6, 0)) == "2026-10-18");
}

TEST_CASE("zona horaria: sin base de zonas, un nombre pide un desfase", "[addon_settings][zona]") {
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

TEST_CASE("zona horaria: un nombre inválido es un error claro", "[addon_settings][zona]") {
    for (const char* name : {"", "Marte/Base", "America/Ciudad_de_Mexico", "UTC+6"}) {
        const auto result = day_in_zone(name);
        REQUIRE(result.is_error());
        CHECK(result.error().kind == chatbot::ErrorKind::Config);
        CHECK(result.error().message.find("Zona horaria") != std::string::npos);
        CHECK(result.error().message.find("-06:00") != std::string::npos);
    }
}
