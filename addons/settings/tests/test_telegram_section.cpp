// La sección "addons" → "telegram" de config.json: lectura estricta,
// versión y escritura sin perder lo demás.

#include "chatbot/addon_settings/telegram_section.h"

#include "temp_dir.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace settings = chatbot::addon_settings;

namespace {

void write(const std::filesystem::path& path, const std::string& text) {
    std::ofstream file{path, std::ios::binary};
    file << text;
}

std::string read(const std::filesystem::path& path) {
    std::ifstream file{path, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

settings::TelegramSectionRead read_section(const std::string& config) {
    const chatbot_test::ScopedTempDir dir;
    const std::filesystem::path path = dir.path() / "config.json";
    write(path, config);
    auto result = settings::read_telegram_section(path.string());
    REQUIRE(result.is_ok());
    return result.value();
}

} // namespace

TEST_CASE("sección: sin archivo o sin sección son los valores por defecto, apagado",
          "[addon_settings][seccion]") {
    const chatbot_test::ScopedTempDir dir;
    const auto missing = settings::read_telegram_section((dir.path() / "no.json").string());
    REQUIRE(missing.is_ok());
    CHECK_FALSE(missing.value().present);
    CHECK_FALSE(missing.value().problem().has_value());
    CHECK(missing.value().values == settings::TelegramSection{});
    CHECK_FALSE(missing.value().values.enabled);

    for (const char* config : {R"({"model": "m"})", R"({"addons": {}})",
                               R"({"addons": {"otro": {"x": 1}}})"}) {
        INFO(config);
        const auto read = read_section(config);
        CHECK_FALSE(read.present);
        CHECK_FALSE(read.problem().has_value());
        CHECK(read.values == settings::TelegramSection{});
    }
}

TEST_CASE("sección: completa y de la versión 1", "[addon_settings][seccion]") {
    const auto read = read_section(R"({"addons": {"telegram": {
        "version": 1, "enabled": true, "model": "otro-modelo",
        "allowed_users": [123456789, 987654321], "invite_code": "Codigo_de_8",
        "daily_message_limit": 20, "max_concurrent_responses": 2,
        "session_idle_minutes": 30, "stale_message_minutes": 5, "draft_interval_ms": 500,
        "time_zone": "America/Tijuana", "log_error_details": true}}})");
    CHECK(read.present);
    CHECK(read.version == 1);
    CHECK(read.discarded.empty());
    CHECK_FALSE(read.problem().has_value());
    const settings::TelegramSection& v = read.values;
    CHECK(v.enabled);
    CHECK(v.model == "otro-modelo");
    CHECK(v.allowed_users == std::vector<std::int64_t>{123456789, 987654321});
    CHECK(v.invite_code == "Codigo_de_8");
    CHECK(v.daily_message_limit == 20);
    CHECK(v.max_concurrent_responses == 2);
    CHECK(v.session_idle_minutes == 30);
    CHECK(v.stale_message_minutes == 5);
    CHECK(v.draft_interval_ms == 500);
    CHECK(v.time_zone == "America/Tijuana");
    CHECK(v.log_error_details);
}

TEST_CASE("sección: llaves desconocidas, de otro tipo o sin version", "[addon_settings][seccion]") {
    SECTION("sin version") {
        const auto read = read_section(R"({"addons": {"telegram": {"enabled": true}}})");
        CHECK(read.present);
        CHECK(read.discarded == std::vector<std::string>{"version"});
        CHECK(read.values.enabled); // Lo que se pudo leer, para F2.
        REQUIRE(read.problem().has_value());
        CHECK(read.problem()->find("\"version\"") != std::string::npos);
    }
    SECTION("llave desconocida") {
        const auto read =
            read_section(R"({"addons": {"telegram": {"version": 1, "enable": true}}})");
        CHECK(read.discarded == std::vector<std::string>{"enable"});
        REQUIRE(read.problem().has_value());
        CHECK(read.problem()->find("\"enable\"") != std::string::npos);
    }
    SECTION("tipos equivocados") {
        const auto read = read_section(R"({"addons": {"telegram": {"version": 1,
            "enabled": "si", "model": 3, "allowed_users": ["123"], "invite_code": null,
            "daily_message_limit": "50", "max_concurrent_responses": 2.5,
            "session_idle_minutes": true, "stale_message_minutes": [], "draft_interval_ms": {},
            "time_zone": 6, "log_error_details": 1}}})");
        CHECK(read.discarded ==
              std::vector<std::string>{"enabled", "model", "allowed_users", "invite_code",
                                       "daily_message_limit", "max_concurrent_responses",
                                       "session_idle_minutes", "stale_message_minutes",
                                       "draft_interval_ms", "time_zone", "log_error_details"});
        CHECK(read.values == settings::TelegramSection{});
        CHECK(read.problem().has_value());
    }
    SECTION("version de otro tipo, 0 o negativa") {
        for (const char* version : {"\"1\"", "1.0", "0", "-1", "99999999999999999999"}) {
            INFO(version);
            const auto read = read_section(std::string{R"({"addons": {"telegram": {"version": )"} +
                                           version + "}}}");
            CHECK(read.discarded == std::vector<std::string>{"version"});
            CHECK(read.problem().has_value());
        }
    }
    SECTION("addons o telegram que no son objetos") {
        CHECK(read_section(R"({"addons": []})").discarded == std::vector<std::string>{"addons"});
        const auto read = read_section(R"({"addons": {"telegram": true}})");
        CHECK(read.discarded == std::vector<std::string>{"telegram"});
        CHECK(read.problem().has_value());
    }
    SECTION("números fuera de rango se leen: los rangos los revisa limits.h") {
        const auto read = read_section(
            R"({"addons": {"telegram": {"version": 1, "daily_message_limit": -4, "allowed_users": [0]}}})");
        CHECK(read.discarded.empty());
        CHECK(read.values.daily_message_limit == -4);
        CHECK(read.values.allowed_users == std::vector<std::int64_t>{0});
    }
}

TEST_CASE("sección: una versión mayor es solo lectura y el bot no arranca",
          "[addon_settings][seccion]") {
    const auto read = read_section(
        R"({"addons": {"telegram": {"version": 2, "enabled": true, "nueva": 1}}})");
    CHECK(read.newer());
    CHECK(read.version == 2);
    REQUIRE(read.problem().has_value());
    CHECK(read.problem()->find("versión 2") != std::string::npos);
    CHECK(read.problem()->find("entiende la 1") != std::string::npos);
}

TEST_CASE("sección: config.json inválido es un error", "[addon_settings][seccion]") {
    const chatbot_test::ScopedTempDir dir;
    const std::filesystem::path path = dir.path() / "config.json";
    write(path, "{ roto");
    CHECK(settings::read_telegram_section(path.string()).is_error());
    CHECK(settings::write_telegram_section(path.string(), {}).has_value());
    CHECK(read(path) == "{ roto");
}

TEST_CASE("sección: se escribe en el orden del esquema sin perder lo demás",
          "[addon_settings][seccion]") {
    const chatbot_test::ScopedTempDir dir;
    const std::filesystem::path path = dir.path() / "config.json";
    write(path, R"({"provider": "nvidia", "addons": {"otro": {"z": 1}, "telegram": {"vieja": 1}},
                    "model": "m"})");
    settings::TelegramSection section;
    section.enabled = true;
    section.allowed_users = {5, 7};
    section.invite_code = "Codigo_de_8";
    section.time_zone = "-06:00";
    REQUIRE_FALSE(settings::write_telegram_section(path.string(), section).has_value());
    CHECK(read(path) == "{\n"
                        "    \"provider\": \"nvidia\",\n"
                        "    \"addons\": {\n"
                        "        \"otro\": {\n"
                        "            \"z\": 1\n"
                        "        },\n"
                        "        \"telegram\": {\n"
                        "            \"version\": 1,\n"
                        "            \"enabled\": true,\n"
                        "            \"allowed_users\": [\n"
                        "                5,\n"
                        "                7\n"
                        "            ],\n"
                        "            \"invite_code\": \"Codigo_de_8\",\n"
                        "            \"daily_message_limit\": 50,\n"
                        "            \"max_concurrent_responses\": 4,\n"
                        "            \"session_idle_minutes\": 60,\n"
                        "            \"stale_message_minutes\": 10,\n"
                        "            \"draft_interval_ms\": 1000,\n"
                        "            \"time_zone\": \"-06:00\",\n"
                        "            \"log_error_details\": false\n"
                        "        }\n"
                        "    },\n"
                        "    \"model\": \"m\"\n"
                        "}\n");

    // Ida y vuelta, con modelo propio.
    section.model = "modelo-del-bot";
    REQUIRE_FALSE(settings::write_telegram_section(path.string(), section).has_value());
    const auto back = settings::read_telegram_section(path.string());
    REQUIRE(back.is_ok());
    CHECK(back.value().discarded.empty());
    CHECK(back.value().values == section);
}

TEST_CASE("sección: enabled false se guarda y se lee", "[addon_settings][seccion]") {
    const chatbot_test::ScopedTempDir dir;
    const std::filesystem::path path = dir.path() / "config.json";
    settings::TelegramSection section;
    section.enabled = false;
    REQUIRE_FALSE(settings::write_telegram_section(path.string(), section).has_value());
    const auto back = settings::read_telegram_section(path.string());
    REQUIRE(back.is_ok());
    CHECK(back.value().present);
    CHECK_FALSE(back.value().problem().has_value());
    CHECK_FALSE(back.value().values.enabled);
}
