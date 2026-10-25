#ifndef CHATBOT_ADDON_SETTINGS_SECRETS_H
#define CHATBOT_ADDON_SETTINGS_SECRETS_H

#include "chatbot/config.h"
#include "chatbot/credentials.h"

#include <string>
#include <string_view>

/// Los secretos del bot de Telegram: en el credentials.json de la app (nunca
/// en config.json) o en el entorno. Son las únicas variables de entorno que
/// lee el bot: ninguna otra CHAT_* (tampoco CHAT_DEBUG_SSE).
namespace chatbot::addon_settings {

inline constexpr std::string_view kTokenEnv = "CHAT_TELEGRAM_TOKEN";
inline constexpr std::string_view kApiKeyEnv = "CHAT_API_KEY";
inline constexpr std::string_view kSearchKeyEnv = "CHAT_SEARCH_API_KEY";

/// El token de @BotFather.
inline constexpr std::string_view kTokenCredentialsKey = "telegram";
/// Key propia del bot para el modelo (opcional).
inline constexpr std::string_view kBotApiKeyCredentialsKey = "telegram:api_key";
/// Key propia del bot para Tavily (opcional).
inline constexpr std::string_view kBotSearchKeyCredentialsKey = "telegram:search_key";

/// La llave de credentials, o vacía si no está.
[[nodiscard]] std::string credential(const Credentials& credentials, std::string_view key);

/// Token: CHAT_TELEGRAM_TOKEN > "telegram". Vacío si no hay.
[[nodiscard]] std::string resolve_token(const EnvLookup& env, const Credentials& credentials);

/// Key del modelo: CHAT_API_KEY > "telegram:api_key" > app_key (la de la app
/// para su proveedor, de load_config). Vacía si no hay.
[[nodiscard]] std::string resolve_model_key(const EnvLookup& env, const Credentials& credentials,
                                            std::string_view app_key);

/// Key de búsqueda: CHAT_SEARCH_API_KEY > "telegram:search_key" >
/// "search:tavily" (la de la app). Vacía: sin /buscar.
[[nodiscard]] std::string resolve_search_key(const EnvLookup& env,
                                             const Credentials& credentials);

} // namespace chatbot::addon_settings

#endif // CHATBOT_ADDON_SETTINGS_SECRETS_H
