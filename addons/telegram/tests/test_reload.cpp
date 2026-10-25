// SIGHUP: reload_settings vuelve a leer config.json, credentials.json e
// invitados.json y lo aplica en caliente, o se queda con la configuración de
// antes. Con archivos de verdad en una carpeta temporal, un Telegram falso y
// un modelo falso.

#include "telegram_harness.hpp"

#include "chatbot/telegram/outbox.h"
#include "chatbot/telegram/settings.h"
#include "chatbot/telegram/startup.h"

#include "chatbot/addon_settings/invited.h"
#include "chatbot/addon_settings/paths.h"
#include "chatbot/bot/bot.h"

#include "signal_watcher.h"

#include "bot_harness.hpp"
#include "fake_transport.hpp"
#include "performance.hpp"
#include "temp_dir.hpp"

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <signal.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>

namespace {

using chatbot::telegram::ReloadResult;
using nlohmann::json;

constexpr const char* kKey = "nvapi-clave-ficticia-de-prueba";
constexpr const char* kSearchKey = "tvly-clave-ficticia-de-prueba";

json section(json changes = json::object()) {
    json out{{"version", 1}, {"enabled", true}, {"allowed_users", {1}}};
    out.update(changes);
    return out;
}

struct ReloadHarness {
    chatbot_test::ScopedTempDir dir;
    chatbot_test::FakeEnv env;
    chatbot::telegram::SettingsPaths paths{(dir.path() / "config.json").string(),
                                           (dir.path() / "credentials.json").string(),
                                           (dir.path() / "telegram").string()};
    std::shared_ptr<chatbot_telegram_test::FakeTelegram> server =
        std::make_shared<chatbot_telegram_test::FakeTelegram>();
    chatbot::telegram::BotApi api{std::string{chatbot_telegram_test::kToken},
                                  chatbot_telegram_test::transports(server)};
    chatbot_bot_test::MemoryLog log;
    chatbot::CancelToken shutdown;
    chatbot::telegram::TelegramOutbox outbox{api, log, shutdown};
    chatbot_bot_test::FakeClock clock;
    std::shared_ptr<chatbot_bot_test::Recorder> recorder =
        std::make_shared<chatbot_bot_test::Recorder>();
    chatbot::telegram::TransportFactory model_transport = [recorder = recorder] {
        return std::make_unique<chatbot_bot_test::ScriptedTransport>(recorder,
                                                                     chatbot_bot_test::echo);
    };
    chatbot::telegram::TelegramSettings current;
    std::unique_ptr<chatbot::bot::Bot> bot;

    ReloadHarness() {
        env.values["CHAT_TELEGRAM_TOKEN"] = std::string{chatbot_telegram_test::kToken};
        env.values["CHAT_API_KEY"] = kKey;
    }

    void write_config(const json& telegram) {
        const json app{{"provider", "nvidia"},
                       {"base_url", "https://integrate.api.nvidia.com/v1"},
                       {"model", "meta/llama-3.3-70b-instruct"},
                       {"addons", {{"telegram", telegram}}}};
        std::ofstream{paths.config, std::ios::binary} << app.dump(2);
    }

    void start(const json& telegram = section()) {
        write_config(telegram);
        auto loaded = chatbot::telegram::load_telegram_settings(paths, std::cref(env));
        REQUIRE(loaded.is_ok());
        current = loaded.value();
        bot = std::make_unique<chatbot::bot::Bot>(
            chatbot_bot_test::valid(current.bot),
            chatbot::bot::BotDependencies{
                chatbot::telegram::model_factory(current, model_transport),
                chatbot::telegram::search_factory(current, model_transport), outbox, clock, log});
    }

    ReloadResult reload() {
        return chatbot::telegram::reload_settings(
            paths, std::cref(env), current,
            chatbot::telegram::ReloadTargets{*bot, outbox, api, shutdown, model_transport}, log);
    }

    /// Manda un mensaje y espera; true si llegó al modelo.
    bool reaches_model(chatbot::bot::ChatId chat, chatbot::bot::UserId user) {
        const std::size_t before = recorder->count();
        bot->handle(chatbot_bot_test::message(chat, user, "hola"));
        REQUIRE(bot->wait_idle(chatbot_test::kThreadTimeout));
        return recorder->count() > before;
    }

    /// Ninguna línea del registro lleva un secreto.
    void check_no_secrets() const {
        for (const std::string& line : log.lines()) {
            INFO(line);
            CHECK(line.find(chatbot_telegram_test::kToken) == std::string::npos);
            CHECK(line.find(kKey) == std::string::npos);
            CHECK(line.find(kSearchKey) == std::string::npos);
        }
    }
};

} // namespace

TEST_CASE("recarga de Telegram: aplica sin perder las conversaciones", "[telegram][recarga]") {
    ReloadHarness h;
    h.start();
    CHECK(h.reaches_model(10, 1));
    CHECK_FALSE(h.reaches_model(20, 2));

    h.write_config(section({{"allowed_users", {1, 2}}, {"log_error_details", true}}));
    CHECK(h.reload() == ReloadResult::Applied);
    CHECK(h.log.contains("config_recargada"));
    CHECK(h.current.bot.allowed_users == std::vector<chatbot::bot::UserId>{1, 2});
    CHECK(h.reaches_model(20, 2));
    CHECK(h.bot->session_count() == 2); // La del 1 sigue.
    CHECK(h.server->calls("setMyCommands").empty()); // /buscar no cambió.
    h.check_no_secrets();
}

TEST_CASE("recarga de Telegram: algo inválido deja la configuración de antes",
          "[telegram][recarga]") {
    ReloadHarness h;
    h.start();
    h.write_config(section({{"allowed_users", {1, 2}}, {"daily_message_limit", 0}}));
    CHECK(h.reload() == ReloadResult::Kept);
    CHECK(h.log.level_of("config_no_recargada") == chatbot::bot::LogLevel::Error);
    CHECK_FALSE(h.log.contains("config_recargada"));
    CHECK_FALSE(h.reaches_model(20, 2));

    h.write_config(section({{"version", 2}}));
    CHECK(h.reload() == ReloadResult::Kept);
    h.write_config(section({{"time_zone", "Marte/Olympus"}}));
    CHECK(h.reload() == ReloadResult::Kept);

    std::filesystem::create_directories(h.paths.data_dir);
    std::ofstream{chatbot::addon_settings::invited_file_path(h.paths.data_dir)} << "{ roto";
    h.write_config(section({{"allowed_users", {1, 2}}}));
    CHECK(h.reload() == ReloadResult::Kept);
    CHECK_FALSE(h.reaches_model(20, 2));
    CHECK(h.reaches_model(10, 1));
    h.check_no_secrets();
}

TEST_CASE("recarga de Telegram: el token necesita reiniciar", "[telegram][recarga]") {
    ReloadHarness h;
    h.start();
    h.env.values["CHAT_TELEGRAM_TOKEN"] = "987654321:AAOtroTokenFicticioDePrueba_xyz12345";
    CHECK(h.reload() == ReloadResult::Applied);
    CHECK(h.log.level_of("requiere_reinicio campo=token") == chatbot::bot::LogLevel::Warning);
    CHECK(h.current.token == chatbot_telegram_test::kToken);
    CHECK(h.log.lines().back().find("987654321") == std::string::npos);

    h.write_config(section({{"max_concurrent_responses", 2}}));
    CHECK(h.reload() == ReloadResult::Applied);
    CHECK(h.log.contains("requiere_reinicio campo=max_concurrent_responses"));
}

TEST_CASE("recarga de Telegram: desactivado apaga el bot", "[telegram][recarga]") {
    ReloadHarness h;
    h.start();
    h.write_config(section({{"enabled", false}}));
    CHECK(h.reload() == ReloadResult::Disable);
    CHECK(h.log.contains("desactivado_por_recarga"));
}

TEST_CASE("recarga de Telegram: /buscar aparece y desaparece del menú", "[telegram][recarga]") {
    ReloadHarness h;
    h.start();
    h.env.values["CHAT_SEARCH_API_KEY"] = kSearchKey;
    CHECK(h.reload() == ReloadResult::Applied);
    REQUIRE(h.server->calls("setMyCommands").size() == 1);
    CHECK(h.server->calls("setMyCommands")[0].body.dump().find("buscar") != std::string::npos);
    h.env.values.erase("CHAT_SEARCH_API_KEY");
    CHECK(h.reload() == ReloadResult::Applied);
    REQUIRE(h.server->calls("setMyCommands").size() == 2);
    CHECK(h.server->calls("setMyCommands")[1].body.dump().find("buscar") == std::string::npos);
    h.check_no_secrets();
}

TEST_CASE("recarga de Telegram: el invitado que quitó la app ya no entra",
          "[telegram][recarga]") {
    ReloadHarness h;
    const std::string invited = chatbot::addon_settings::invited_file_path(h.paths.data_dir);
    REQUIRE(chatbot::addon_settings::add_invited(invited, 7).is_ok());
    h.start(section({{"allowed_users", json::array()}, {"invite_code", "codigo-de-prueba"}}));
    // El Bot de la prueba arranca sin invitados: los toma del archivo al recargar.
    CHECK(h.reload() == ReloadResult::Applied);
    CHECK(h.reaches_model(70, 7));
    REQUIRE(chatbot::addon_settings::remove_invited(invited, 7).is_ok());
    CHECK(h.reload() == ReloadResult::Applied);
    CHECK_FALSE(h.reaches_model(70, 7));
    CHECK(h.bot->session_count() == 0);
}

TEST_CASE("recarga de Telegram: SIGTERM corta un setMyCommands colgado", "[telegram][recarga]") {
    sigset_t old_mask;
    pthread_sigmask(SIG_SETMASK, nullptr, &old_mask);
    {
        ReloadHarness h;
        // Antes del bot, como en main: sus hilos heredan las señales bloqueadas.
        std::optional<chatbot::telegram::SignalWatcher> signals;
        signals.emplace(h.shutdown, h.log);
        h.start();
        h.server->block("setMyCommands");
        h.env.values["CHAT_SEARCH_API_KEY"] = kSearchKey; // /buscar aparece: setMyCommands.
        signals->watch(h.bot.get(), [&h] { return h.reload() != ReloadResult::Disable; });

        signals->send(SIGHUP);
        REQUIRE(h.server->wait_for("setMyCommands", 1));
        const auto begin = std::chrono::steady_clock::now();
        signals->send(SIGTERM);
        REQUIRE(h.shutdown.wait_for(chatbot_test::kThreadTimeout));
        signals.reset(); // Espera la recarga y el apagado del bot.
        chatbot_test::report_latency("apagado durante una recarga",
                                     std::chrono::steady_clock::now() - begin);
        CHECK(h.log.contains("apagando senal=SIGTERM"));
        CHECK_FALSE(h.log.contains("comandos_no_puestos")); // Cancelado no es un fallo.
        h.check_no_secrets();
    }
    pthread_sigmask(SIG_SETMASK, &old_mask, nullptr);
}
