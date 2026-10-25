// Configuración del bot: la sección "addons" → "telegram" del config.json de
// la app, sus archivos y sus secretos (las reglas de cada pieza se prueban en
// addons/settings/tests).

#include "chatbot/telegram/settings.h"

#include "chatbot/addon_settings/bot_model.h"
#include "chatbot/addon_settings/time_zone.h"

#include "env_guard.hpp"
#include "fake_transport.hpp"
#include "posix_permissions.hpp"
#include "temp_dir.hpp"

#include "chatbot/credentials.h"
#include "chatbot/system_prompt.h"

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <string>

namespace {

using chatbot::telegram::load_telegram_settings;
using nlohmann::json;

constexpr const char* kToken = "123456789:AAFakeTokenForTestsOnly_abcdefghijk";
constexpr const char* kKey = "nvapi-clave-ficticia-de-prueba";

void write(const std::filesystem::path& path, const std::string& text) {
    std::ofstream file{path, std::ios::binary};
    file << text;
}

struct SettingsHarness {
    chatbot_test::ScopedTempDir dir;
    chatbot_test::FakeEnv env;
    chatbot::telegram::SettingsPaths paths{(dir.path() / "config.json").string(),
                                           (dir.path() / "credentials.json").string(),
                                           (dir.path() / "telegram").string()};

    SettingsHarness() {
        env.values["CHAT_TELEGRAM_TOKEN"] = kToken;
        env.values["CHAT_API_KEY"] = kKey;
    }

    /// config.json de la app con la sección del bot (sin section: sin sección).
    chatbot::Result<chatbot::telegram::TelegramSettings> load(json app, const json& section) {
        if (!section.is_null()) {
            app["addons"]["telegram"] = section;
        }
        write(paths.config, app.dump(2));
        return load_telegram_settings(paths, std::cref(env));
    }
};

json app_config() {
    return json{{"provider", "nvidia"},
                {"base_url", "https://integrate.api.nvidia.com/v1"},
                {"model", "meta/llama-3.3-70b-instruct"}};
}

json enabled() {
    return json{{"version", 1}, {"enabled", true}};
}

} // namespace

TEST_CASE("configuración: sin sección o sin config.json, desactivado", "[telegram][config]") {
    SettingsHarness h;
    h.env.values.clear(); // Desactivado no pide token ni key.
    auto result = load_telegram_settings(h.paths, std::cref(h.env));
    REQUIRE(result.is_ok());
    CHECK_FALSE(result.value().enabled);
    result = h.load(app_config(), json{});
    REQUIRE(result.is_ok());
    CHECK_FALSE(result.value().enabled);
    result = h.load(app_config(), json{{"version", 1}, {"enabled", false}});
    REQUIRE(result.is_ok());
    CHECK_FALSE(result.value().enabled);
}

TEST_CASE("configuración: lo de la app y los valores por defecto", "[telegram][config]") {
    SettingsHarness h;
    json app = app_config();
    app["history_limit"] = 9000;
    app["timeout_seconds"] = 30;
    const auto result = h.load(app, enabled());
    REQUIRE(result.is_ok());
    const auto& settings = result.value();
    CHECK(settings.enabled);
    CHECK(settings.model.model == "meta/llama-3.3-70b-instruct");
    CHECK(settings.model.base_url == chatbot::kDefaultBaseUrl);
    CHECK(settings.model.api_key == kKey);
    CHECK(settings.model.timeout_seconds == std::chrono::seconds{30});
    CHECK(settings.bot.history_limit_bytes == 9000);
    CHECK(settings.token == kToken);
    CHECK(settings.search_api_key.empty());
    CHECK(settings.time_zone == chatbot::addon_settings::default_time_zone());
    CHECK(settings.bot.system_prompt == chatbot::kDefaultSystemPrompt);
    CHECK_FALSE(settings.system_prompt_invalid);
    CHECK(settings.bot.allowed_users.empty());
    CHECK(settings.bot.daily_message_limit == 50);
    CHECK(settings.bot.max_concurrent_responses == 4);
    CHECK(settings.bot.session_idle_timeout == std::chrono::minutes{60});
    CHECK(settings.bot.stale_after == std::chrono::minutes{10});
    CHECK(settings.bot.draft_interval == std::chrono::milliseconds{1000});
    CHECK_FALSE(settings.bot.log_error_details);
    CHECK(chatbot::bot::validate_bot_config(settings.bot).is_ok());
}

TEST_CASE("configuración: todas las llaves de la sección", "[telegram][config]") {
    SettingsHarness h;
    json app = app_config();
    app["system_prompt"] = "";
    const auto result = h.load(app, json{{"version", 1},
                                         {"enabled", true},
                                         {"model", "otro/modelo"},
                                         {"allowed_users", {111, 222}},
                                         {"invite_code", "codigo_del_profe"},
                                         {"daily_message_limit", 30},
                                         {"max_concurrent_responses", 2},
                                         {"session_idle_minutes", 90},
                                         {"stale_message_minutes", 5},
                                         {"draft_interval_ms", 1500},
                                         {"time_zone", "-07:00"},
                                         {"log_error_details", true}});
    REQUIRE(result.is_ok());
    const auto& bot = result.value().bot;
    CHECK(result.value().model.model == "otro/modelo");
    CHECK(bot.system_prompt.empty());
    CHECK(bot.allowed_users == std::vector<chatbot::bot::UserId>{111, 222});
    CHECK(bot.invite_code == "codigo_del_profe");
    CHECK(bot.daily_message_limit == 30);
    CHECK(bot.max_concurrent_responses == 2);
    CHECK(bot.session_idle_timeout == std::chrono::minutes{90});
    CHECK(bot.stale_after == std::chrono::minutes{5});
    CHECK(bot.draft_interval == std::chrono::milliseconds{1500});
    CHECK(bot.log_error_details);
    CHECK(result.value().time_zone == "-07:00");
}

TEST_CASE("configuración: una sección que no se entiende es error", "[telegram][config]") {
    struct Case {
        json section;
        const char* expected;
    };
    const std::vector<Case> cases = {
        {json{{"enabled", true}}, "\"version\""},
        {json{{"version", 1}, {"enabled", true}, {"modelo", "x"}}, "\"modelo\""},
        {json{{"version", 1}, {"enabled", "si"}}, "\"enabled\""},
        // También desactivada: así F2 y el bot ven lo mismo.
        {json{{"version", 1}, {"enabled", false}, {"daily_message_limit", "50"}},
         "\"daily_message_limit\""},
        {json{{"version", 2}, {"enabled", true}}, "versión 2"},
    };
    for (const Case& c : cases) {
        SettingsHarness h;
        INFO(c.section.dump());
        const auto result = h.load(app_config(), c.section);
        REQUIRE(result.is_error());
        CHECK(result.error().kind == chatbot::ErrorKind::Config);
        CHECK(result.error().message.find(c.expected) != std::string::npos);
    }
    SettingsHarness h;
    write(h.paths.config, "{ esto no es json");
    const auto broken = load_telegram_settings(h.paths, std::cref(h.env));
    REQUIRE(broken.is_error());
    CHECK(broken.error().kind == chatbot::ErrorKind::Config);
}

TEST_CASE("configuración: los rangos los revisa validate_bot_config", "[telegram][config]") {
    SettingsHarness h;
    json section = enabled();
    section["daily_message_limit"] = -1;
    const auto result = h.load(app_config(), section);
    REQUIRE(result.is_ok()); // Se leyó; validate_bot_config lo rechaza.
    const auto valid = chatbot::bot::validate_bot_config(result.value().bot);
    REQUIRE(valid.is_error());
    CHECK(valid.error().message.find("10000") != std::string::npos);
}

TEST_CASE("configuración: system_prompt de otro tipo usa el predeterminado",
          "[telegram][config]") {
    SettingsHarness h;
    json app = app_config();
    app["system_prompt"] = 5;
    const auto result = h.load(app, enabled());
    REQUIRE(result.is_ok());
    CHECK(result.value().system_prompt_invalid);
    CHECK(result.value().bot.system_prompt == chatbot::kDefaultSystemPrompt);
}

TEST_CASE("configuración: token y keys del entorno o de credentials.json", "[telegram][config]") {
    SettingsHarness h;
    h.env.values.clear();
    // Sin token: dice dónde ponerlo.
    auto result = h.load(app_config(), enabled());
    REQUIRE(result.is_error());
    CHECK(result.error().message.find("Configuración → Addons") != std::string::npos);
    CHECK(result.error().message.find("CHAT_TELEGRAM_TOKEN") != std::string::npos);

    chatbot::Credentials credentials;
    credentials.keys["telegram"] = kToken;
    credentials.keys["nvidia"] = kKey;
    credentials.keys["search:tavily"] = "tvly-de-la-app";
    REQUIRE_FALSE(chatbot::save_credentials(h.paths.credentials, credentials).has_value());
    result = h.load(app_config(), enabled());
    REQUIRE(result.is_ok());
    CHECK(result.value().token == kToken);
    CHECK(result.value().model.api_key == kKey);
    CHECK(result.value().search_api_key == "tvly-de-la-app");

    // Las keys propias del bot ganan a las de la app; el entorno, a todas.
    credentials.keys["telegram:api_key"] = "nvapi-propia-del-bot";
    credentials.keys["telegram:search_key"] = "tvly-propia-del-bot";
    REQUIRE_FALSE(chatbot::save_credentials(h.paths.credentials, credentials).has_value());
    result = h.load(app_config(), enabled());
    REQUIRE(result.is_ok());
    CHECK(result.value().model.api_key == "nvapi-propia-del-bot");
    CHECK(result.value().search_api_key == "tvly-propia-del-bot");
    h.env.values["CHAT_SEARCH_API_KEY"] = "tvly-del-entorno";
    result = h.load(app_config(), enabled());
    REQUIRE(result.is_ok());
    CHECK(result.value().search_api_key == "tvly-del-entorno");

    // Sin provider en config.json: sin key, con el texto de la decisión 11.
    h.env.values.clear();
    credentials.keys.erase("telegram:api_key");
    REQUIRE_FALSE(chatbot::save_credentials(h.paths.credentials, credentials).has_value());
    json app = app_config();
    app.erase("provider");
    result = h.load(app, enabled());
    REQUIRE(result.is_error());
    CHECK(result.error().message == chatbot::addon_settings::kMissingModelKey);

#ifndef _WIN32
    // credentials.json con permisos abiertos: no se lee.
    chatbot_test::set_mode(h.paths.credentials, 0644);
    result = h.load(app_config(), enabled());
    REQUIRE(result.is_error());
    CHECK(result.error().message.find("chmod 600") != std::string::npos);
#endif
}

TEST_CASE("configuración: un token mal copiado se rechaza sin mostrarlo", "[telegram][config]") {
    SettingsHarness h;
    h.env.values["CHAT_TELEGRAM_TOKEN"] = "123456:corto/getMe?x";
    const auto result = h.load(app_config(), enabled());
    REQUIRE(result.is_error());
    CHECK(result.error().message.find("@BotFather") != std::string::npos);
    CHECK(result.error().message.find("corto") == std::string::npos);
}

TEST_CASE("configuración: las rutas de este proceso son las de la app", "[telegram][config]") {
    const chatbot_test::EnvGuard config_home{"XDG_CONFIG_HOME", "/cfg"};
    const chatbot_test::EnvGuard data_home{"XDG_DATA_HOME", "/datos"};
    const auto paths = chatbot::telegram::default_settings_paths();
    REQUIRE(paths.is_ok());
    CHECK(paths.value().config == chatbot::default_config_path());
    CHECK(paths.value().credentials == chatbot::default_credentials_path());
#ifndef _WIN32
    CHECK(paths.value().config == "/cfg/chatbot/config.json");
    CHECK(paths.value().data_dir == "/datos/chatbot/telegram");
#endif
}
