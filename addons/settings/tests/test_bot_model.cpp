// El modelo y los secretos del bot: la misma función que la app
// (load_config), las keys propias del bot y el entorno.

#include "chatbot/addon_settings/bot_model.h"
#include "chatbot/addon_settings/secrets.h"

#include "chatbot/config.h"
#include "chatbot/credentials.h"

#include "temp_dir.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>

namespace settings = chatbot::addon_settings;

namespace {

/// config.json y credentials.json de la app en una carpeta temporal.
struct AppFiles {
    chatbot_test::ScopedTempDir dir;
    std::string config = (dir.path() / "config.json").string();
    std::string credentials = (dir.path() / "credentials.json").string();
    std::map<std::string, std::string> env;

    void write_config(const std::string& text) const {
        std::ofstream file{config, std::ios::binary};
        file << text;
    }
    void write_keys(std::map<std::string, std::string> keys) const {
        REQUIRE_FALSE(chatbot::save_credentials(credentials, chatbot::Credentials{std::move(keys)})
                          .has_value());
    }
    [[nodiscard]] chatbot::EnvLookup lookup() const {
        return [this](std::string_view name) {
            const auto found = env.find(std::string{name});
            return found != env.end() ? std::string_view{found->second} : std::string_view{};
        };
    }
    [[nodiscard]] chatbot::Credentials keys() const {
        auto loaded = chatbot::load_credentials(credentials);
        REQUIRE(loaded.is_ok());
        return loaded.value();
    }
    /// La key a la que llega la app (allow_missing: como al arrancar la TUI).
    [[nodiscard]] std::string app_key() const {
        chatbot::ConfigOptions options;
        options.env = lookup();
        options.path = [this] { return std::optional<std::string>{config}; };
        options.credentials_path = [this] { return std::optional<std::string>{credentials}; };
        options.allow_missing_key_and_model = true;
        auto loaded = chatbot::load_config(options);
        REQUIRE(loaded.is_ok());
        return loaded.value().api_key;
    }
    [[nodiscard]] chatbot::Result<chatbot::Config> bot(std::string_view model = "") const {
        return settings::resolve_bot_model(config, credentials, keys(), model, lookup());
    }
};

} // namespace

TEST_CASE("modelo del bot: con los mismos archivos llega a la misma key que la app",
          "[addon_settings][modelo]") {
    AppFiles files;
    files.write_config(R"({"provider": "nvidia", "base_url": "https://integrate.api.nvidia.com/v1",
                           "model": "meta/llama-3.3-70b-instruct", "timeout_seconds": 30,
                           "history_limit": 9000})");
    files.write_keys({{"nvidia", "nvapi-de-la-app"}, {"search:tavily", "tvly-app"}});
    const auto bot = files.bot();
    REQUIRE(bot.is_ok());
    CHECK(bot.value().api_key == files.app_key());
    CHECK(bot.value().api_key == "nvapi-de-la-app");
    CHECK(bot.value().provider == "nvidia");
    CHECK(bot.value().model == "meta/llama-3.3-70b-instruct");
    CHECK(bot.value().timeout_seconds == std::chrono::seconds{30});
    CHECK(bot.value().history_limit_bytes == 9000);

    // Con CHAT_API_KEY también llegan a la misma.
    files.env["CHAT_API_KEY"] = "nvapi-del-entorno";
    REQUIRE(files.bot().is_ok());
    CHECK(files.bot().value().api_key == files.app_key());
    CHECK(files.app_key() == "nvapi-del-entorno");
}

TEST_CASE("modelo del bot: sin provider, ni la app ni el bot tienen key",
          "[addon_settings][modelo]") {
    AppFiles files;
    // Escrito a mano, sin "provider": load_config no busca en credentials.json.
    files.write_config(R"({"model": "m"})");
    files.write_keys({{"nvidia", "nvapi-de-la-app"}, {"custom:https://integrate.api.nvidia.com/v1",
                                                      "nvapi-custom"}});
    CHECK(files.app_key().empty());
    const auto bot = files.bot();
    REQUIRE(bot.is_error());
    CHECK(bot.error().kind == chatbot::ErrorKind::Config);
    CHECK(bot.error().message ==
          "Falta la key del modelo: elige el proveedor y guarda la key en Configuración → "
          "Proveedor.");
}

TEST_CASE("modelo del bot: prioridad de la key y modelo propio", "[addon_settings][modelo]") {
    AppFiles files;
    files.write_config(R"({"provider": "nvidia", "model": "de-la-app"})");
    files.write_keys({{"nvidia", "app"}, {"telegram:api_key", "propia"}});
    REQUIRE(files.bot().is_ok());
    CHECK(files.bot().value().api_key == "propia"); // telegram:api_key > la de la app.
    files.env["CHAT_API_KEY"] = "entorno";
    CHECK(files.bot().value().api_key == "entorno"); // CHAT_API_KEY > telegram:api_key.

    CHECK(files.bot().value().model == "de-la-app");
    CHECK(files.bot("del-bot").value().model == "del-bot");
}

TEST_CASE("modelo del bot: ninguna otra CHAT_* cuenta", "[addon_settings][modelo]") {
    AppFiles files;
    files.write_config(R"({"provider": "nvidia", "model": "de-la-app"})");
    files.write_keys({{"nvidia", "app"}});
    files.env["CHAT_MODEL"] = "del-entorno";
    files.env["CHAT_BASE_URL"] = "https://otro.example/v1";
    files.env["CHAT_TIMEOUT"] = "5";
    files.env["CHAT_HISTORY_LIMIT"] = "1";
    files.env["CHAT_DEBUG_SSE"] = "/tmp/volcado";
    const auto bot = files.bot();
    REQUIRE(bot.is_ok());
    CHECK(bot.value().model == "de-la-app");
    CHECK(bot.value().base_url == "https://integrate.api.nvidia.com/v1");
    CHECK(bot.value().timeout_seconds == std::chrono::seconds{120});
    CHECK(bot.value().history_limit_bytes == 32000);
    CHECK_FALSE(bot.value().debug_sse_path.has_value());
}

TEST_CASE("modelo del bot: sin modelo ni en la app ni propio", "[addon_settings][modelo]") {
    AppFiles files;
    files.write_config(R"({"provider": "nvidia"})");
    files.write_keys({{"nvidia", "app"}});
    const auto bot = files.bot();
    REQUIRE(bot.is_error());
    CHECK(bot.error().message.find("Falta el modelo") != std::string::npos);
    CHECK(files.bot("propio").is_ok());
}

TEST_CASE("secretos: token y key de búsqueda, con su prioridad", "[addon_settings][secretos]") {
    std::map<std::string, std::string> env;
    const chatbot::EnvLookup lookup = [&env](std::string_view name) {
        const auto found = env.find(std::string{name});
        return found != env.end() ? std::string_view{found->second} : std::string_view{};
    };
    chatbot::Credentials keys;
    CHECK(settings::resolve_token(lookup, keys).empty());
    CHECK(settings::resolve_search_key(lookup, keys).empty());

    keys.keys["telegram"] = "123:archivo";
    keys.keys["search:tavily"] = "tvly-app";
    CHECK(settings::resolve_token(lookup, keys) == "123:archivo");
    CHECK(settings::resolve_search_key(lookup, keys) == "tvly-app");
    keys.keys["telegram:search_key"] = "tvly-bot";
    CHECK(settings::resolve_search_key(lookup, keys) == "tvly-bot");
    env["CHAT_TELEGRAM_TOKEN"] = "123:entorno";
    env["CHAT_SEARCH_API_KEY"] = "tvly-entorno";
    CHECK(settings::resolve_token(lookup, keys) == "123:entorno");
    CHECK(settings::resolve_search_key(lookup, keys) == "tvly-entorno");
    CHECK(settings::resolve_model_key(lookup, keys, "app") == "app");
}
