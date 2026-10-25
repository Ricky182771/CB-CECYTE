#ifndef CHATBOT_TELEGRAM_SETTINGS_H
#define CHATBOT_TELEGRAM_SETTINGS_H

#include "chatbot/bot/config.h"
#include "chatbot/bot/types.h"

#include "chatbot/config.h"
#include "chatbot/result.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace chatbot::telegram {

/// Variables de entorno del bot. Solo los secretos: lo demás va en el
/// archivo de configuración.
inline constexpr std::string_view kTokenEnv = "CHAT_TELEGRAM_TOKEN";
inline constexpr std::string_view kApiKeyEnv = "CHAT_API_KEY";
inline constexpr std::string_view kSearchKeyEnv = "CHAT_SEARCH_API_KEY";
/// Ruta del archivo de configuración si no se pasa --config.
inline constexpr std::string_view kConfigEnv = "CHAT_TELEGRAM_CONFIG";
/// Llave del token en credentials.json.
inline constexpr std::string_view kTokenCredentialsKey = "telegram";

/// Todo lo que el bot necesita, ya leído y revisado.
struct TelegramSettings {
    bot::BotConfig bot;          ///< Falta pasarla por validate_bot_config.
    Config model;                ///< Proveedor, URL, modelo, key (validate_config ya pasó).
    std::string search_api_key;  ///< Vacía: sin /buscar.
    std::string token;           ///< plausible_token ya pasó.
    std::string time_zone;       ///< Sin revisar: day_in_zone.
    std::optional<std::string> data_dir; ///< Donde va invitados.json.
    std::string credentials_path; ///< credentials.json junto a la configuración.
};

/// Lee el archivo de configuración del bot (JSON, sin secretos) y los
/// secretos (entorno; si no, credentials.json en la misma carpeta, con
/// permisos 600 como el del núcleo). Llaves del archivo, todas opcionales
/// salvo model: provider, base_url, model, system_prompt, allowed_users,
/// invite_code, daily_message_limit, max_concurrent_responses,
/// session_idle_minutes, history_limit, time_zone, stale_message_minutes,
/// draft_interval_ms, data_dir (obligatoria con invite_code) y
/// log_error_details. Una llave desconocida o de otro tipo es un error
/// (así se nota un error de dedo). Los errores son Config, en español, con
/// la ruta y la llave; nunca con un secreto.
[[nodiscard]] Result<TelegramSettings> load_telegram_settings(const std::string& config_path,
                                                              const EnvLookup& env);

} // namespace chatbot::telegram

#endif // CHATBOT_TELEGRAM_SETTINGS_H
