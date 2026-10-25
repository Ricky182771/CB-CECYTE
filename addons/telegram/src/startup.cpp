#include "chatbot/telegram/startup.h"

#include "chatbot/addon_settings/invited.h"
#include "chatbot/addon_settings/paths.h"
#include "chatbot/addon_settings/time_zone.h"

#include "chatbot/chat_client.h"
#include "chatbot/platform.h"
#include "chatbot/tavily_search.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>

namespace chatbot::telegram {

std::vector<BotCommandInfo> bot_commands(bool search_available) {
    std::vector<BotCommandInfo> commands{
        {"start", "Empezar (o entrar con un código de invitación)"},
        {"ayuda", "Ver qué puedo hacer"},
        {"nuevo", "Empezar una conversación nueva"},
    };
    if (search_available) {
        commands.push_back({"buscar", "Buscar en la web y responder con las fuentes"});
    }
    return commands;
}

Result<BotIdentity> connect_to_telegram(BotApi& api, bot::Log& log, const CancelToken& shutdown,
                                        Sleeper* sleeper, std::chrono::seconds max_backoff) {
    RealSleeper real_sleeper;
    Sleeper& waiter = sleeper != nullptr ? *sleeper : real_sleeper;
    for (unsigned failures = 0;; ++failures) {
        Result<BotIdentity> identity = api.get_me(&shutdown);
        if (identity.is_ok()) {
            if (failures > 0) {
                log.write(bot::LogLevel::Info,
                          "conexion_recuperada fallos=" + std::to_string(failures));
            }
            return identity;
        }
        const ChatError& error = identity.error();
        if (error.kind == ErrorKind::Cancelled || shutdown.is_cancelled()) {
            return ChatError{ErrorKind::Cancelled, 0, "cancelado", std::nullopt};
        }
        if (error.kind == ErrorKind::Auth) {
            return ChatError{ErrorKind::Auth, error.http_status,
                             "Telegram no aceptó el token (HTTP " +
                                 std::to_string(error.http_status) +
                                 "). Revisa que sea el que te dio @BotFather.",
                             std::nullopt};
        }
        if (failures == 0) {
            log.write(bot::LogLevel::Warning,
                      "sin_conexion tipo=\"" + std::string{error_kind_label(error.kind)} +
                          "\" http=" + std::to_string(error.http_status));
        }
        const std::chrono::seconds wait =
            error.kind == ErrorKind::RateLimited && error.retry_after.has_value()
                ? *error.retry_after
                : std::min(max_backoff,
                           std::chrono::seconds{std::int64_t{1} << std::min(failures, 16U)});
        if (!waiter.sleep_for(wait, &shutdown)) {
            return ChatError{ErrorKind::Cancelled, 0, "cancelado", std::nullopt};
        }
    }
}

bot::ClientFactory model_factory(const TelegramSettings& settings,
                                TransportFactory make_transport) {
    return [model = settings.model, make_transport = std::move(make_transport)] {
        return std::make_unique<ChatClient>(model, make_transport());
    };
}

bot::SearchFactory search_factory(const TelegramSettings& settings,
                                  TransportFactory make_transport) {
    if (settings.search_api_key.empty()) {
        return {};
    }
    return [key = settings.search_api_key, make_transport = std::move(make_transport)] {
        return std::unique_ptr<SearchProvider>{std::make_unique<TavilySearch>(key, make_transport())};
    };
}

ReloadResult reload_settings(const SettingsPaths& paths, const EnvLookup& env,
                             TelegramSettings& current, const ReloadTargets& targets,
                             bot::Log& log) {
    const auto keep = [&log](const std::string& why) {
        std::string reason = why;
        for (char& c : reason) {
            c = c == '"' ? '\'' : c;
        }
        log.write(bot::LogLevel::Error, "config_no_recargada motivo=\"" + reason + "\"");
        return ReloadResult::Kept;
    };
    Result<TelegramSettings> loaded = load_telegram_settings(paths, env);
    if (loaded.is_error()) {
        return keep(loaded.error().message);
    }
    TelegramSettings next = loaded.value();
    if (!next.enabled) {
        log.write(bot::LogLevel::Info, "desactivado_por_recarga");
        return ReloadResult::Disable;
    }
    Result<addon_settings::DayFunction> day = addon_settings::day_in_zone(next.time_zone);
    if (day.is_error()) {
        return keep(day.error().message);
    }
    Result<bot::ValidBotConfig> config = bot::validate_bot_config(next.bot);
    if (config.is_error()) {
        return keep(config.error().message);
    }
    Result<std::vector<bot::UserId>> invited =
        addon_settings::load_invited(addon_settings::invited_file_path(paths.data_dir));
    if (invited.is_error()) {
        return keep(invited.error().message);
    }

    if (next.token != current.token) {
        log.write(bot::LogLevel::Warning, "requiere_reinicio campo=token");
        next.token = current.token; // BotApi, Poller y el outbox se quedan con el de antes.
    }
    if (next.system_prompt_invalid) {
        log.write(bot::LogLevel::Warning, "system_prompt_invalido");
    }
    const bool had_search = !current.search_api_key.empty();
    const bool search = !next.search_api_key.empty();
    targets.bot.reconfigure(config.value(), model_factory(next, targets.make_transport),
                            search_factory(next, targets.make_transport), day.value(),
                            invited.value());
    targets.outbox.set_log_error_details(next.bot.log_error_details);
    if (search != had_search) {
        if (std::optional<ChatError> error =
                targets.api.set_my_commands(bot_commands(search), &targets.shutdown);
            error.has_value() && error->kind != ErrorKind::Cancelled) { // Cancelado: apagando.
            log.write(bot::LogLevel::Warning,
                      "comandos_no_puestos http=" + std::to_string(error->http_status));
        }
    }
    current = std::move(next);
    log.write(bot::LogLevel::Info, "config_recargada");
    return ReloadResult::Applied;
}

std::optional<ChatError> prepare_data_dir(const std::string& dir) {
    const auto failure = [&dir](const std::string& cause) {
        return ChatError{ErrorKind::Config, 0,
                         "No se puede escribir en la carpeta de datos del bot: " + dir + " (" +
                             cause + ").",
                         std::nullopt};
    };
    if (std::optional<std::string> error =
            create_private_directory(dir, FolderPrivacy::PrivateIfNew)) {
        return failure(*error);
    }
    // Que exista no basta: puede ser de solo lectura.
    const std::string probe = dir + "/.prueba-escritura";
    if (std::optional<std::string> error =
            write_file_atomic(probe, "", FilePrivacy::Private, FolderPrivacy::PrivateIfNew)) {
        return failure(*error);
    }
    std::error_code ignored;
    std::filesystem::remove(probe, ignored); // Si se queda, está vacío y no estorba.
    return std::nullopt;
}

int exit_code_for(const ChatError& error, FailureSource source) {
    switch (source) {
    case FailureSource::Telegram:
        return error.kind == ErrorKind::Auth ? kExitConfig : kExitFailure;
    case FailureSource::DataDir:
        return kExitCantCreate;
    case FailureSource::Invited:
        return error.kind == ErrorKind::BadResponse ? kExitDataError : kExitFailure;
    }
    return kExitFailure;
}

} // namespace chatbot::telegram
