// chatbot-telegram: el bot de Telegram (long polling, sin webhook). Lee la
// configuración de la app (config.json → "addons" → "telegram" y
// credentials.json), revisa el token con getMe, pone el menú de comandos y
// atiende hasta SIGINT o SIGTERM. Se arranca con "chatbot telegram". Códigos
// de salida en settings.h. Solo Linux por ahora (AGENTS.md, sección "Addons").

#include "chatbot/telegram/api.h"
#include "chatbot/telegram/log.h"
#include "chatbot/telegram/outbox.h"
#include "chatbot/telegram/poller.h"
#include "chatbot/telegram/settings.h"
#include "chatbot/telegram/startup.h"

#include "signal_watcher.h"

#include "chatbot/bot/bot.h"
#include "chatbot/bot/config.h"

#include "chatbot/addon_settings/invited.h"
#include "chatbot/addon_settings/paths.h"
#include "chatbot/addon_settings/telegram_section.h"
#include "chatbot/addon_settings/time_zone.h"

#include "chatbot/cancel_token.h"
#include "chatbot/curl_transport.h"
#include "chatbot/platform.h"

#include <signal.h>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace chatbot;
using namespace chatbot::telegram;

constexpr std::string_view kUsage =
    "Uso: chatbot telegram                    arranca el bot\n"
    "     chatbot telegram --config-version   la versión de la configuración que entiende\n"
    "     chatbot telegram --help             esta ayuda\n"
    "\n"
    "Lee la configuración de la app (config.json y credentials.json): se edita\n"
    "en Configuración (F2) → Addons. Ver la sección \"Bot de Telegram\" del README.\n";

constexpr std::string_view kDisabled =
    "El addon de Telegram está desactivado (Configuración → Addons).";

std::string_view real_env(std::string_view name) {
    const char* value = std::getenv(std::string{name}.c_str());
    return value != nullptr ? std::string_view{value} : std::string_view{};
}

int fail(std::string_view message, int code = kExitFailure) {
    std::cerr << "chatbot-telegram: " << message << '\n';
    return code;
}

int run() {
    StreamLog log{std::cerr};
    CancelToken shutdown;
    SignalWatcher signals{shutdown, log};

    Result<SettingsPaths> paths = default_settings_paths();
    if (paths.is_error()) {
        return fail(paths.error().message, kExitConfig);
    }
    Result<TelegramSettings> loaded = load_telegram_settings(paths.value(), real_env);
    if (loaded.is_error()) {
        return fail(loaded.error().message, kExitConfig);
    }
    TelegramSettings settings = loaded.value();
    if (!settings.enabled) {
        std::cerr << kDisabled << '\n';
        return 0;
    }
    if (settings.system_prompt_invalid) {
        log.write(bot::LogLevel::Warning, "system_prompt_invalido");
    }
    Result<addon_settings::DayFunction> day = addon_settings::day_in_zone(settings.time_zone);
    if (day.is_error()) {
        return fail(day.error().message, kExitConfig);
    }
    Result<bot::ValidBotConfig> config = bot::validate_bot_config(settings.bot);
    if (config.is_error()) {
        return fail("Configuración → Addons → Telegram: " + config.error().message, kExitConfig);
    }

    const std::string& data_dir = paths.value().data_dir;
    if (std::optional<ChatError> error = prepare_data_dir(data_dir)) {
        return fail(error->message, exit_code_for(*error, FailureSource::DataDir));
    }
    const std::string invited_path = addon_settings::invited_file_path(data_dir);
    Result<std::vector<bot::UserId>> invited = addon_settings::load_invited(invited_path);
    if (invited.is_error()) {
        return fail(invited.error().message,
                    exit_code_for(invited.error(), FailureSource::Invited));
    }

    const TransportFactory make_transport = [] { return std::make_unique<CurlTransport>(); };
    BotApi api{settings.token, make_transport};
    Result<BotIdentity> identity = connect_to_telegram(api, log, shutdown);
    if (identity.is_error()) {
        const ChatError& error = identity.error();
        if (error.kind == ErrorKind::Cancelled) {
            return 0;
        }
        if (error.kind == ErrorKind::Auth) {
            log.write(bot::LogLevel::Error,
                      "telegram_token_invalido http=" + std::to_string(error.http_status));
        }
        return fail(error.message, exit_code_for(error));
    }
    log.write(bot::LogLevel::Info, "telegram_conectado bot=@" + identity.value().username +
                                       " id=" + std::to_string(identity.value().id));
    const bool search = !settings.search_api_key.empty();
    if (std::optional<ChatError> error = api.set_my_commands(bot_commands(search), &shutdown)) {
        log.write(bot::LogLevel::Warning, "comandos_no_puestos http=" +
                                              std::to_string(error->http_status));
    }

    TelegramOutbox outbox{api, log, shutdown,
                          OutboxOptions{settings.bot.log_error_details}};
    outbox.start();
    bot::SystemClock clock;
    bot::BotDependencies dependencies{model_factory(settings, make_transport),
                                      search_factory(settings, make_transport), outbox, clock,
                                      log};
    dependencies.day = day.value();
    dependencies.invited = invited.value();
    dependencies.add_invited =
        [invited_path, &log](bot::UserId user) -> std::optional<std::vector<bot::UserId>> {
        Result<std::vector<bot::UserId>> saved = addon_settings::add_invited(invited_path, user);
        if (saved.is_error()) {
            log.write(bot::LogLevel::Error, "invitados_error " + saved.error().message);
            return std::nullopt;
        }
        return saved.value();
    };
    bot::Bot bot{config.value(), std::move(dependencies)};
    signals.watch(&bot, [&] {
        return reload_settings(paths.value(), real_env, settings,
                               ReloadTargets{bot, outbox, api, shutdown, make_transport},
                               log) != ReloadResult::Disable;
    });

    Poller poller{api,
                  PollerHandlers{[&bot](const bot::IncomingMessage& message) { bot.handle(message); },
                                 [&bot](bot::ChatId chat, bot::ReplyId reply) {
                                     (void)bot.stop(chat, reply);
                                 },
                                 [&bot] { bot.expire_idle(); }},
                  log, shutdown};
    const std::optional<ChatError> fatal = poller.run();
    signals.watch(nullptr, {});
    shutdown.cancel();
    bot.shutdown();
    outbox.stop();
    if (fatal.has_value()) {
        // El Poller ya anotó telegram_token_invalido.
        return fail("Telegram dejó de aceptar el token (HTTP " +
                        std::to_string(fatal->http_status) +
                        "): revisa que no lo hayan cambiado en @BotFather.",
                    exit_code_for(*fatal));
    }
    log.write(bot::LogLevel::Info, "fin");
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    signal(SIGPIPE, SIG_IGN);
    if (argc > 2) {
        std::cerr << "chatbot-telegram: demasiados argumentos.\n\n" << kUsage;
        return kExitFailure;
    }
    if (argc == 2) {
        const std::string_view argument{argv[1]};
        if (argument == "--help" || argument == "-h") {
            std::cout << kUsage;
            return 0;
        }
        // Sin leer nada ni tocar la red: F2 lo usa para ver si es compatible.
        if (argument == "--config-version") {
            std::cout << addon_settings::kTelegramConfigVersion << '\n';
            return 0;
        }
        std::cerr << "chatbot-telegram: argumento desconocido: " << argument << "\n\n" << kUsage;
        return kExitFailure;
    }
    try {
        return run();
    } catch (const std::exception& e) {
        return fail(std::string{"error inesperado: "} + e.what());
    }
}
