#ifndef CHATBOT_ADDON_SETTINGS_BOT_MODEL_H
#define CHATBOT_ADDON_SETTINGS_BOT_MODEL_H

#include "chatbot/config.h"
#include "chatbot/credentials.h"
#include "chatbot/result.h"

#include <string>
#include <string_view>

namespace chatbot::addon_settings {

/// Sin key del modelo para el bot (ni CHAT_API_KEY, ni "telegram:api_key",
/// ni la de la app para su proveedor; por ejemplo, un config.json escrito a
/// mano sin "provider").
inline constexpr std::string_view kMissingModelKey =
    "Falta la key del modelo: elige el proveedor y guarda la key en Configuración → Proveedor.";
/// Sin modelo en la app ni uno propio del bot.
inline constexpr std::string_view kMissingModel =
    "Falta el modelo: elígelo en Configuración → Proveedor (o uno propio del bot en "
    "Configuración → Addons).";

/// El modelo del bot, resuelto con la misma función que la app (load_config,
/// con config_path y credentials_path, pero sin el entorno: el bot no lee
/// CHAT_BASE_URL, CHAT_MODEL, CHAT_TIMEOUT, CHAT_HISTORY_LIMIT ni
/// CHAT_DEBUG_SSE). Proveedor, URL base, timeout y history_limit, los de la
/// app; el modelo, section_model si no está vacío; la key, resolve_model_key
/// (credentials es el mismo credentials.json). Errores Config:
/// kMissingModelKey, kMissingModel o el de validate_config.
[[nodiscard]] Result<Config> resolve_bot_model(const std::string& config_path,
                                               const std::string& credentials_path,
                                               const Credentials& credentials,
                                               std::string_view section_model,
                                               const EnvLookup& env);

} // namespace chatbot::addon_settings

#endif // CHATBOT_ADDON_SETTINGS_BOT_MODEL_H
