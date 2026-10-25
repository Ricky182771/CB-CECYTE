#include "chatbot/telegram/settings.h"

#include "chatbot/addon_settings/bot_model.h"
#include "chatbot/addon_settings/paths.h"
#include "chatbot/addon_settings/secrets.h"
#include "chatbot/addon_settings/telegram_section.h"
#include "chatbot/addon_settings/time_zone.h"
#include "chatbot/addon_settings/token.h"

#include "chatbot/credentials.h"
#include "chatbot/system_prompt.h"

#include <chrono>
#include <cstddef>
#include <exception>
#include <optional>
#include <utility>

namespace chatbot::telegram {
namespace {

namespace settings = addon_settings;

ChatError config_error(std::string message) {
    return ChatError{ErrorKind::Config, 0, std::move(message), std::nullopt};
}

} // namespace

Result<SettingsPaths> default_settings_paths() {
    std::optional<std::string> config = default_config_path();
    std::optional<std::string> credentials = default_credentials_path();
    std::optional<std::string> data_dir = settings::default_addon_data_dir("telegram");
    if (!config.has_value() || !credentials.has_value() || !data_dir.has_value()) {
        return config_error(missing_config_dir_message(current_os()));
    }
    return SettingsPaths{std::move(*config), std::move(*credentials), std::move(*data_dir)};
}

Result<TelegramSettings> load_telegram_settings(const SettingsPaths& paths, const EnvLookup& env) {
    try {
        Result<settings::TelegramSectionRead> read = settings::read_telegram_section(paths.config);
        if (read.is_error()) {
            return read.error();
        }
        const settings::TelegramSectionRead& section = read.value();
        if (const std::optional<std::string> problem = section.problem()) {
            return config_error(*problem);
        }
        TelegramSettings out;
        const settings::TelegramSection& values = section.values;
        out.enabled = values.enabled;
        if (!out.enabled) {
            return out;
        }

        bot::BotConfig& config = out.bot;
        config.allowed_users = values.allowed_users;
        config.invite_code = values.invite_code;
        config.daily_message_limit = static_cast<std::size_t>(values.daily_message_limit);
        config.max_concurrent_responses =
            static_cast<std::size_t>(values.max_concurrent_responses);
        config.session_idle_timeout = std::chrono::minutes{values.session_idle_minutes};
        config.stale_after = std::chrono::minutes{values.stale_message_minutes};
        config.draft_interval = std::chrono::milliseconds{values.draft_interval_ms};
        config.log_error_details = values.log_error_details;
        out.time_zone = values.time_zone.empty() ? std::string{settings::default_time_zone()}
                                                 : values.time_zone;

        // Las instrucciones, como la app: de otro tipo → las predeterminadas.
        const ResolvedSystemPrompt prompt =
            resolve_system_prompt(load_system_prompt_value(paths.config));
        config.system_prompt = prompt.text;
        out.system_prompt_invalid = !prompt.warning.empty();

        Result<Credentials> credentials = load_credentials(paths.credentials);
        if (credentials.is_error()) {
            return credentials.error();
        }
        out.token = settings::resolve_token(env, credentials.value());
        if (out.token.empty()) {
            return config_error("Falta el token de Telegram: guárdalo en Configuración → "
                                "Addons (o ponlo en " +
                                std::string{settings::kTokenEnv} + ").");
        }
        if (!settings::plausible_token(out.token)) {
            return config_error("El token de Telegram no tiene la forma de uno de @BotFather "
                                "(números, ':' y letras, sin espacios).");
        }
        Result<Config> model = settings::resolve_bot_model(
            paths.config, paths.credentials, credentials.value(), values.model, env);
        if (model.is_error()) {
            return model.error();
        }
        out.model = std::move(model.value());
        config.history_limit_bytes = out.model.history_limit_bytes;
        out.search_api_key = settings::resolve_search_key(env, credentials.value());
        return out;
    } catch (const std::exception&) {
        return config_error("No se pudo leer la configuración del bot (" + paths.config + ").");
    }
}

} // namespace chatbot::telegram
