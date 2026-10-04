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

/// Lo que necesita una petición, sin el modelo: URL base válida, key (salvo
/// para un servidor local) y timeout. Lo usan validate_config y
/// ChatClient::list_models (que se llama antes de elegir modelo).
[[nodiscard]] std::optional<ChatError> validate_connection(const Config& config);

} // namespace chatbot

#endif // CHATBOT_CONFIG_INTERNAL_H
