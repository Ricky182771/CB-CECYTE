// Configuración del bot (archivo JSON + secretos) y el archivo de invitados.

#include "chatbot/telegram/settings.h"

#include "chatbot/addon_settings/time_zone.h"

#include "fake_transport.hpp"
#include "posix_permissions.hpp"
#include "temp_dir.hpp"

#include "chatbot/credentials.h"

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <string>

#ifndef _WIN32
#include <sys/stat.h>
#endif

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
    std::filesystem::path config = dir.path() / "telegram.json";

    SettingsHarness() {
        env.values["CHAT_TELEGRAM_TOKEN"] = kToken;
        env.values["CHAT_API_KEY"] = kKey;
    }

    chatbot::Result<chatbot::telegram::TelegramSettings> load(const json& document) {
        write(config, document.dump(2));
        return load_telegram_settings(config.string(), std::cref(env));
    }
};

json minimal() {
    return json{{"model", "meta/llama-3.3-70b-instruct"}};
}

} // namespace

TEST_CASE("configuración: lo mínimo y los valores por defecto", "[telegram][config]") {
    SettingsHarness h;
    const auto result = h.load(minimal());
    REQUIRE(result.is_ok());
    const auto& settings = result.value();
    CHECK(settings.model.model == "meta/llama-3.3-70b-instruct");
    CHECK(settings.model.base_url == chatbot::kDefaultBaseUrl);
    CHECK(settings.model.api_key == kKey);
    CHECK(settings.token == kToken);
    CHECK(settings.search_api_key.empty());
    CHECK(settings.time_zone == chatbot::addon_settings::default_time_zone());
    CHECK_FALSE(settings.data_dir.has_value());
    CHECK(settings.bot.system_prompt == chatbot::kDefaultSystemPrompt);
    CHECK(settings.bot.allowed_users.empty());
    CHECK(settings.bot.stale_after == std::chrono::minutes{10});
    CHECK(settings.bot.draft_interval == std::chrono::milliseconds{1000});
    CHECK_FALSE(settings.bot.log_error_details);
    CHECK(chatbot::bot::validate_bot_config(settings.bot).is_ok());
    CHECK(settings.credentials_path == (h.dir.path() / "credentials.json").string());
}

TEST_CASE("configuración: todas las llaves", "[telegram][config]") {
    SettingsHarness h;
    const auto result = h.load(json{{"provider", "nvidia"},
                                    {"base_url", "https://integrate.api.nvidia.com/v1"},
                                    {"model", "m"},
                                    {"system_prompt", ""},
                                    {"allowed_users", {111, 222}},
                                    {"invite_code", "codigo_del_profe"},
                                    {"daily_message_limit", 30},
                                    {"max_concurrent_responses", 2},
                                    {"session_idle_minutes", 90},
                                    {"history_limit", 0},
                                    {"time_zone", "America/Tijuana"},
                                    {"stale_message_minutes", 5},
                                    {"draft_interval_ms", 1500},
                                    {"data_dir", (h.dir.path() / "datos").string()},
                                    {"log_error_details", true}});
    REQUIRE(result.is_ok());
    const auto& bot = result.value().bot;
    CHECK(bot.system_prompt.empty());
    CHECK(bot.allowed_users == std::vector<chatbot::bot::UserId>{111, 222});
    CHECK(bot.invite_code == "codigo_del_profe");
    CHECK(bot.daily_message_limit == 30);
    CHECK(bot.max_concurrent_responses == 2);
    CHECK(bot.session_idle_timeout == std::chrono::minutes{90});
    CHECK(bot.history_limit_bytes == 0);
    CHECK(result.value().model.history_limit_bytes == 0);
    CHECK(bot.stale_after == std::chrono::minutes{5});
    CHECK(bot.draft_interval == std::chrono::milliseconds{1500});
    CHECK(bot.log_error_details);
    CHECK(result.value().time_zone == "America/Tijuana");
    CHECK(result.value().data_dir == (h.dir.path() / "datos").string());
}

TEST_CASE("configuración: errores claros", "[telegram][config]") {
    struct Case {
        json document;
        const char* expected;
    };
    const std::vector<Case> cases = {
        {json::object(), "\"model\" falta"},
        {json{{"model", "m"}, {"modelo", "x"}}, "\"modelo\" no es una opción"},
        {json{{"model", 5}}, "\"model\" debe ser texto"},
        {json{{"model", "m"}, {"allowed_users", "123"}}, "\"allowed_users\" debe ser una lista"},
        {json{{"model", "m"}, {"allowed_users", {"123"}}}, "solo puede tener IDs"},
        {json{{"model", "m"}, {"allowed_users", {-5}}}, "solo puede tener IDs"},
        {json{{"model", "m"}, {"daily_message_limit", -1}}, "\"daily_message_limit\" debe ser un número"},
        {json{{"model", "m"}, {"daily_message_limit", 2.5}}, "debe ser un número entero"},
        {json{{"model", "m"}, {"log_error_details", "si"}}, "debe ser true o false"},
        {json{{"model", "m"}, {"base_url", "http://ejemplo.mx/v1"}}, "\"base_url\" no es válida"},
        {json{{"model", "m"}, {"invite_code", "codigo_del_profe"}}, "\"data_dir\" falta"},
        {json{{"model", "m"}, {"data_dir", "datos"}}, "ruta completa"},
    };
    for (const Case& c : cases) {
        SettingsHarness h;
        const auto result = h.load(c.document);
        INFO(c.document.dump());
        REQUIRE(result.is_error());
        CHECK(result.error().kind == chatbot::ErrorKind::Config);
        CHECK(result.error().message.find(c.expected) != std::string::npos);
        CHECK(result.error().message.find(h.config.string()) != std::string::npos);
    }
    SettingsHarness h;
    write(h.config, "{ esto no es json");
    const auto broken = load_telegram_settings(h.config.string(), std::cref(h.env));
    REQUIRE(broken.is_error());
    CHECK(broken.error().message.find("no es JSON válido") != std::string::npos);
    const auto missing = load_telegram_settings((h.dir.path() / "no.json").string(), std::cref(h.env));
    REQUIRE(missing.is_error());
    CHECK(missing.error().message.find("No se pudo abrir") != std::string::npos);
}

TEST_CASE("configuración: secretos del entorno o de credentials.json", "[telegram][config]") {
    SettingsHarness h;
    h.env.values.clear();
    // Sin token ni key: dice dónde ponerlos, sin mostrar nada secreto.
    auto result = h.load(minimal());
    REQUIRE(result.is_error());
    CHECK(result.error().message.find("CHAT_TELEGRAM_TOKEN") != std::string::npos);
    CHECK(result.error().message.find("credentials.json") != std::string::npos);

    chatbot::Credentials credentials;
    credentials.keys["telegram"] = kToken;
    credentials.keys["nvidia"] = kKey;
    credentials.keys["search:tavily"] = "tvly-ficticia";
    REQUIRE_FALSE(chatbot::save_credentials((h.dir.path() / "credentials.json").string(), credentials)
                      .has_value());
    json document = minimal();
    document["provider"] = "nvidia";
    result = h.load(document);
    REQUIRE(result.is_ok());
    CHECK(result.value().token == kToken);
    CHECK(result.value().model.api_key == kKey);
    CHECK(result.value().search_api_key == "tvly-ficticia");

    // El entorno gana.
    h.env.values["CHAT_SEARCH_API_KEY"] = "tvly-del-entorno";
    result = h.load(document);
    REQUIRE(result.is_ok());
    CHECK(result.value().search_api_key == "tvly-del-entorno");

    // Falta la key del proveedor: dice qué llave buscar.
    document["provider"] = "openrouter";
    h.env.values.clear();
    result = h.load(document);
    REQUIRE(result.is_error());
    CHECK(result.error().message.find("\"openrouter\"") != std::string::npos);
    CHECK(result.error().message.find(kKey) == std::string::npos);
    // Un servidor local no necesita key.
    document["provider"] = "custom";
    document["base_url"] = "http://localhost:11434/v1";
    CHECK(h.load(document).is_ok());

#ifndef _WIN32
    // credentials.json con permisos abiertos: no se lee.
    chatbot_test::set_mode(h.dir.path() / "credentials.json", 0644);
    result = h.load(minimal());
    REQUIRE(result.is_error());
    CHECK(result.error().message.find("chmod 600") != std::string::npos);
#endif
}

TEST_CASE("configuración: un token mal copiado se rechaza sin mostrarlo", "[telegram][config]") {
    SettingsHarness h;
    h.env.values["CHAT_TELEGRAM_TOKEN"] = "123456:corto/getMe?x";
    const auto result = h.load(minimal());
    REQUIRE(result.is_error());
    CHECK(result.error().message.find("@BotFather") != std::string::npos);
    CHECK(result.error().message.find("corto") == std::string::npos);
}

