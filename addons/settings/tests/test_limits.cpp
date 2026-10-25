// Rangos de la configuración de los bots: un validador por campo, con el
// texto que muestra F2 debajo del campo.

#include "chatbot/addon_settings/limits.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace settings = chatbot::addon_settings;

TEST_CASE("límites: cada número en su rango, con los extremos", "[addon_settings][limites]") {
    struct Case {
        std::optional<std::string> (*check)(std::int64_t);
        std::int64_t min;
        std::int64_t max;
        const char* words;
    };
    const Case cases[] = {
        {settings::check_idle_minutes, 1, 1440, "inactiva debe ser de 1 a 1440 minutos."},
        {settings::check_daily_limit, 1, 10000, "por día debe ser de 1 a 10000."},
        {settings::check_concurrent_responses, 1, 32, "simultáneas deben ser de 1 a 32."},
        {settings::check_stale_minutes, 1, 1440, "viejo un mensaje deben ser de 1 a 1440."},
        {settings::check_draft_interval_ms, 250, 10000, "vista previa debe ser de 250 a 10000 ms."},
    };
    for (const Case& c : cases) {
        INFO(c.words);
        CHECK_FALSE(c.check(c.min).has_value());
        CHECK_FALSE(c.check(c.max).has_value());
        for (const std::int64_t bad : {c.min - 1, c.max + 1, std::int64_t{-5}}) {
            const auto error = c.check(bad);
            REQUIRE(error.has_value());
            CHECK(error->find(c.words) != std::string::npos);
        }
    }
}

TEST_CASE("límites: IDs mayores que 0", "[addon_settings][limites]") {
    CHECK_FALSE(settings::check_allowed_users({}).has_value());
    CHECK_FALSE(settings::check_allowed_users({1, 123456789}).has_value());
    const auto error = settings::check_allowed_users({5, 0});
    REQUIRE(error.has_value());
    CHECK(error->find("(0)") != std::string::npos);
    CHECK(settings::check_allowed_users({-3}).has_value());
}

TEST_CASE("límites: el código de invitación, sin repetirlo en el error",
          "[addon_settings][limites]") {
    CHECK_FALSE(settings::check_invite_code("").has_value());
    CHECK_FALSE(settings::check_invite_code("Abc_12-z").has_value());
    CHECK_FALSE(settings::check_invite_code(std::string(64, 'a')).has_value());
    for (const std::string& bad : {std::string{"corto7c"}, std::string(65, 'a'),
                                  std::string{"con espacio"}, std::string{"acentuadó"}}) {
        const auto error = settings::check_invite_code(bad);
        REQUIRE(error.has_value());
        CHECK(error->find(bad) == std::string::npos);
    }
}
