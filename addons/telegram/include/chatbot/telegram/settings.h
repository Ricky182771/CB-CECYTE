#ifndef CHATBOT_TELEGRAM_SETTINGS_H
#define CHATBOT_TELEGRAM_SETTINGS_H

#include "chatbot/bot/config.h"

#include "chatbot/config.h"
#include "chatbot/result.h"

#include <string>

namespace chatbot::telegram {

/// Códigos de salida de chatbot-telegram (0: apagado normal o desactivado).
/// Lo inesperado (excepciones y errores que no se clasifican): la unidad de
/// systemd sí reinicia con él.
inline constexpr int kExitFailure = 1;
/// Los demás son de sysexits.h y no se arreglan solos: la unidad no
/// reinicia con ellos (RestartPreventExitStatus).
/// EX_DATAERR: invitados.json dañado.
inline constexpr int kExitDataError = 65;
/// EX_CANTCREAT: la carpeta de datos del bot no se puede crear o escribir.
inline constexpr int kExitCantCreate = 73;
/// EX_CONFIG: configuración inválida, versión distinta, falta la key o el
/// token no sirve (también si Telegram lo rechaza ya corriendo).
inline constexpr int kExitConfig = 78;

/// Los archivos que lee el bot: los mismos de la app.
struct SettingsPaths {
    std::string config;      ///< config.json de la app.
    std::string credentials; ///< credentials.json de la app.
    std::string data_dir;    ///< Carpeta de datos del bot (invitados.json).
};

/// Las de este proceso: default_config_path, default_credentials_path (con
/// el modo portable, como la app) y default_addon_data_dir("telegram").
/// Error Config si falta HOME.
[[nodiscard]] Result<SettingsPaths> default_settings_paths();

/// Todo lo que el bot necesita, ya leído y revisado.
struct TelegramSettings {
    bool enabled = false;        ///< false: lo demás se queda sin llenar.
    bot::BotConfig bot;          ///< Falta pasarla por validate_bot_config.
    Config model;                ///< El de la app (o el modelo propio); validate_config ya pasó.
    std::string search_api_key;  ///< Vacía: sin /buscar.
    std::string token;           ///< plausible_token ya pasó.
    std::string time_zone;       ///< Sin revisar: day_in_zone.
    /// system_prompt de config.json no era una cadena: se usan las
    /// predeterminadas, como en la app (el registro lo avisa).
    bool system_prompt_invalid = false;
};

/// Lee la sección "addons" → "telegram" de config.json (estricta: una llave
/// desconocida, de otro tipo, sin "version" o de una versión mayor es
/// error); si está desactivada, solo eso. Si no: el modelo con
/// resolve_bot_model (proveedor, URL, timeout e history_limit de la app),
/// las instrucciones de system_prompt como la app, y los secretos de
/// secrets.h (entorno: solo CHAT_TELEGRAM_TOKEN, CHAT_API_KEY y
/// CHAT_SEARCH_API_KEY; si no, credentials.json). Todos los errores son
/// Config (salida 78), en español y sin secretos.
[[nodiscard]] Result<TelegramSettings> load_telegram_settings(const SettingsPaths& paths,
                                                              const EnvLookup& env);

} // namespace chatbot::telegram

#endif // CHATBOT_TELEGRAM_SETTINGS_H
