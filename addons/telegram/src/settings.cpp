#include "chatbot/telegram/settings.h"

#include "chatbot/telegram/api.h"

#include "chatbot/addon_settings/time_zone.h"
#include "chatbot/addon_settings/token.h"

#include "chatbot/credentials.h"
#include "chatbot/platform.h"
#include "chatbot/system_prompt.h"

#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <utility>

namespace chatbot::telegram {
namespace {

using nlohmann::json;

/// Las llaves que entiende el archivo de configuración.
constexpr std::array<std::string_view, 15> kKnownKeys{
    "provider",          "base_url",          "model",
    "system_prompt",     "allowed_users",     "invite_code",
    "daily_message_limit", "max_concurrent_responses", "session_idle_minutes",
    "history_limit",     "time_zone",         "stale_message_minutes",
    "draft_interval_ms", "data_dir",          "log_error_details"};

/// Tope de un número del archivo (los rangos los revisa
/// validate_bot_config; esto solo evita desbordes).
constexpr std::uint64_t kMaxNumber = 100000000;

ChatError config_error(std::string message) {
    return ChatError{ErrorKind::Config, 0, std::move(message), std::nullopt};
}

/// Lee el JSON del archivo; los errores dicen la ruta.
class FileValues {
public:
    FileValues(std::string path, json document)
        : path_(std::move(path)), document_(std::move(document)) {}

    [[nodiscard]] ChatError error(std::string_view key, std::string_view problem) const {
        return config_error(path_ + ": \"" + std::string{key} + "\" " + std::string{problem});
    }

    [[nodiscard]] bool has(std::string_view key) const {
        return document_.contains(std::string{key});
    }

    [[nodiscard]] Result<std::string> text(std::string_view key, std::string fallback) const {
        if (!has(key)) {
            return fallback;
        }
        const json& value = document_.at(std::string{key});
        if (!value.is_string()) {
            return error(key, "debe ser texto entre comillas.");
        }
        return value.get<std::string>();
    }

    [[nodiscard]] Result<std::uint64_t> number(std::string_view key, std::uint64_t fallback) const {
        if (!has(key)) {
            return fallback;
        }
        const json& value = document_.at(std::string{key});
        if (!value.is_number_unsigned() || value.get<std::uint64_t>() > kMaxNumber) {
            return error(key, "debe ser un número entero, sin comillas, de 0 a " +
                                  std::to_string(kMaxNumber) + ".");
        }
        return value.get<std::uint64_t>();
    }

    [[nodiscard]] Result<bool> flag(std::string_view key, bool fallback) const {
        if (!has(key)) {
            return fallback;
        }
        const json& value = document_.at(std::string{key});
        if (!value.is_boolean()) {
            return error(key, "debe ser true o false.");
        }
        return value.get<bool>();
    }

    [[nodiscard]] Result<std::vector<bot::UserId>> ids(std::string_view key) const {
        std::vector<bot::UserId> out;
        if (!has(key)) {
            return out;
        }
        const json& value = document_.at(std::string{key});
        if (!value.is_array()) {
            return error(key, "debe ser una lista de IDs, por ejemplo [123456789].");
        }
        for (const json& item : value) {
            if (!item.is_number_integer() || item.get<std::int64_t>() <= 0) {
                return error(key, "solo puede tener IDs de Telegram: números enteros mayores "
                                  "que 0, sin comillas.");
            }
            out.push_back(item.get<std::int64_t>());
        }
        return out;
    }

    [[nodiscard]] std::optional<std::string> unknown_key() const {
        for (auto it = document_.begin(); it != document_.end(); ++it) {
            bool known = false;
            for (const std::string_view candidate : kKnownKeys) {
                known = known || it.key() == candidate;
            }
            if (!known) {
                return it.key();
            }
        }
        return std::nullopt;
    }

private:
    std::string path_;
    json document_;
};

/// Un secreto: la variable de entorno o, si no, la llave de credentials.json
/// (que se lee una sola vez, solo si hace falta).
class Secrets {
public:
    Secrets(const EnvLookup& env, std::string credentials_path)
        : env_(env), path_(std::move(credentials_path)) {}

    [[nodiscard]] Result<std::string> get(std::string_view variable, const std::string& key) {
        if (env_) {
            const std::string_view value = env_(variable);
            if (!value.empty()) {
                return std::string{value};
            }
        }
        if (!loaded_.has_value()) {
            Result<Credentials> credentials = load_credentials(path_);
            if (credentials.is_error()) {
                return credentials.error();
            }
            loaded_ = std::move(credentials.value());
        }
        const auto found = loaded_->keys.find(key);
        return found != loaded_->keys.end() ? found->second : std::string{};
    }

private:
    const EnvLookup& env_;
    std::string path_;
    std::optional<Credentials> loaded_;
};

} // namespace

Result<TelegramSettings> load_telegram_settings(const std::string& config_path,
                                                const EnvLookup& env) {
    try {
        std::ifstream file{config_path, std::ios::binary};
        if (!file) {
            return config_error("No se pudo abrir el archivo de configuración del bot: " +
                                config_path + ".");
        }
        std::ostringstream content;
        content << file.rdbuf();
        json document;
        try {
            document = json::parse(content.str());
        } catch (const json::exception& e) {
            return config_error(config_path + " no es JSON válido (" + std::string{e.what()} +
                                ").");
        }
        if (!document.is_object()) {
            return config_error(config_path +
                                " debe ser un objeto JSON: { \"model\": \"...\", ... }.");
        }
        const FileValues values{config_path, document};
        if (const std::optional<std::string> unknown = values.unknown_key()) {
            return values.error(*unknown, "no es una opción del bot (¿un error de dedo?).");
        }

        TelegramSettings settings;
        bot::BotConfig& config = settings.bot;
        // Se lee todo y se devuelve el primer error.
        std::optional<ChatError> error;
        const auto read = [&error](auto& target, const auto& result) {
            if (error.has_value()) {
                return;
            }
            if (result.is_error()) {
                error = result.error();
            } else {
                target = result.value();
            }
        };
        const auto count = [&values](std::string_view key, auto current) {
            return values.number(key, static_cast<std::uint64_t>(current));
        };
        std::string provider;
        std::uint64_t daily = 0;
        std::uint64_t concurrent = 0;
        std::uint64_t idle = 0;
        std::uint64_t history = 0;
        std::uint64_t stale = 0;
        std::uint64_t draft = 0;
        read(provider, values.text("provider", ""));
        read(settings.model.base_url, values.text("base_url", std::string{kDefaultBaseUrl}));
        read(settings.model.model, values.text("model", ""));
        read(config.system_prompt, values.text("system_prompt", std::string{kDefaultSystemPrompt}));
        read(config.allowed_users, values.ids("allowed_users"));
        read(config.invite_code, values.text("invite_code", ""));
        read(daily, count("daily_message_limit", config.daily_message_limit));
        read(concurrent, count("max_concurrent_responses", config.max_concurrent_responses));
        read(idle, count("session_idle_minutes", config.session_idle_timeout.count()));
        read(history, count("history_limit", config.history_limit_bytes));
        read(stale, count("stale_message_minutes", config.stale_after.count()));
        read(draft, count("draft_interval_ms", config.draft_interval.count()));
        read(config.log_error_details, values.flag("log_error_details", false));
        read(settings.time_zone, values.text("time_zone", std::string{addon_settings::default_time_zone()}));
        if (error.has_value()) {
            return *error;
        }
        if (const std::optional<ChatError> problem = validate_base_url(settings.model.base_url)) {
            return values.error("base_url", "no es válida: " + problem->message);
        }
        if (settings.model.model.empty()) {
            return values.error("model", "falta: escribe el nombre del modelo (no hay uno por "
                                         "defecto, porque los proveedores los retiran).");
        }
        settings.model.provider = provider;
        config.daily_message_limit = static_cast<std::size_t>(daily);
        config.max_concurrent_responses = static_cast<std::size_t>(concurrent);
        config.session_idle_timeout = std::chrono::minutes{static_cast<std::int64_t>(idle)};
        config.history_limit_bytes = static_cast<std::size_t>(history);
        settings.model.history_limit_bytes = config.history_limit_bytes;
        config.stale_after = std::chrono::minutes{static_cast<std::int64_t>(stale)};
        config.draft_interval = std::chrono::milliseconds{static_cast<std::int64_t>(draft)};

        if (values.has("data_dir")) {
            std::string dir;
            read(dir, values.text("data_dir", ""));
            if (error.has_value()) {
                return *error;
            }
            if (dir.empty() || !std::filesystem::path{dir}.is_absolute()) {
                return values.error("data_dir", "debe ser una ruta completa (que empiece con /).");
            }
            settings.data_dir = dir;
        } else if (!config.invite_code.empty()) {
            return values.error("data_dir", "falta: con invite_code, el bot guarda ahí los IDs de "
                                            "quienes usan el código.");
        }

        // Secretos: entorno o credentials.json (600) junto al archivo.
        const std::filesystem::path folder =
            std::filesystem::absolute(std::filesystem::path{config_path}).parent_path();
        settings.credentials_path = (folder / "credentials.json").string();
        Secrets secrets{env, settings.credentials_path};

        Result<std::string> token = secrets.get(kTokenEnv, std::string{kTokenCredentialsKey});
        if (token.is_error()) {
            return token.error();
        }
        if (token.value().empty()) {
            return config_error("Falta el token de Telegram: ponlo en " + std::string{kTokenEnv} +
                                " o en " + settings.credentials_path + " con la llave \"" +
                                std::string{kTokenCredentialsKey} + "\".");
        }
        if (!addon_settings::plausible_token(token.value())) {
            return config_error("El token de Telegram no tiene la forma de uno de @BotFather "
                                "(números, ':' y letras, sin espacios).");
        }
        settings.token = std::move(token.value());

        const std::string model_key = credentials_key(provider.empty() ? "custom" : provider,
                                                      settings.model.base_url);
        Result<std::string> api_key = secrets.get(kApiKeyEnv, model_key);
        if (api_key.is_error()) {
            return api_key.error();
        }
        settings.model.api_key = std::move(api_key.value());
        if (const std::optional<ChatError> problem = validate_config(settings.model)) {
            if (settings.model.api_key.empty()) {
                return config_error("Falta la key del modelo: ponla en " +
                                    std::string{kApiKeyEnv} + " o en " +
                                    settings.credentials_path + " con la llave \"" + model_key +
                                    "\".");
            }
            return *problem;
        }

        Result<std::string> search = secrets.get(kSearchKeyEnv, std::string{kSearchCredentialsKey});
        if (search.is_error()) {
            return search.error();
        }
        settings.search_api_key = std::move(search.value());
        return settings;
    } catch (const std::exception&) {
        return config_error("No se pudo leer la configuración del bot (" + config_path + ").");
    }
}

} // namespace chatbot::telegram
