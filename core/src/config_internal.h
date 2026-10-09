#ifndef CHATBOT_CONFIG_INTERNAL_H
#define CHATBOT_CONFIG_INTERNAL_H

#include "chatbot/config.h"

#include <optional>
#include <string>

namespace chatbot {

/// Construye la ruta del archivo de configuración a partir de XDG_CONFIG_HOME
/// y $HOME, para poder probar la regla XDG sin mutar el entorno del proceso.
/// - Con XDG_CONFIG_HOME presente y no vacío: $XDG_CONFIG_HOME/chatbot/config.json
/// - Si no, con HOME: $HOME/.config/chatbot/config.json
/// - Si no hay nada: nullopt.
/// Header interno (src/); no forma parte de la API pública.
[[nodiscard]] std::optional<std::string> build_config_path(
    const std::optional<std::string>& xdg_config_home,
    const std::optional<std::string>& home);

/// Construye la ruta del archivo de configuración en Windows, donde HOME y
/// XDG_* no cuentan (MSYS2 define HOME y la configuración quedaría partida
/// según la shell):
/// - <roaming_app_data>\chatbot\config.json, con la carpeta AppData\Roaming
///   que da Windows (known_folder(KnownFolder::RoamingAppData));
/// - si no la dio, con la variable APPDATA (appdata);
/// - si no hay ninguna: nullopt.
/// Función pura, para probarla también en Linux.
[[nodiscard]] std::optional<std::string> build_windows_config_path(
    const std::optional<std::string>& roaming_app_data,
    const std::optional<std::string>& appdata);

/// Lo que necesita una petición, sin el modelo: URL base válida, key (salvo
/// para un servidor local) y timeout. Lo usan validate_config y
/// ChatClient::list_models (que se llama antes de elegir modelo).
[[nodiscard]] std::optional<ChatError> validate_connection(const Config& config);

} // namespace chatbot

#endif // CHATBOT_CONFIG_INTERNAL_H
