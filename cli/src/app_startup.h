#ifndef CHATBOT_CLI_APP_STARTUP_H
#define CHATBOT_CLI_APP_STARTUP_H

#include "session.h"

#include "chatbot/config.h"
#include "chatbot/result.h"

#include <string_view>

namespace chatbot::cli {

// Arranque común de la TUI (main.cpp) y de chatbot-rpc, para que las dos
// interfaces carguen lo mismo de los mismos lugares.

/// Modo portable (prepare_portable_data_dir si hay chatbot.portable) y
/// load_config con allow_missing_key_and_model. El error es el que la TUI
/// imprime al salir (y el RPC devuelve en initialize). Que solo falte la key
/// o el modelo no es error: lo dice validate_config.
[[nodiscard]] Result<Config> load_startup_config();

/// El entorno real del proceso (getenv); una variable que no existe da "".
[[nodiscard]] std::string_view process_env(std::string_view name);

/// SessionEnvironment con el sistema real: rutas por defecto (también en
/// modo portable), el entorno del proceso, CurlTransport, TavilySearch, el
/// reloj y la carpeta de descargas.
[[nodiscard]] SessionEnvironment app_session_environment();

} // namespace chatbot::cli

#endif // CHATBOT_CLI_APP_STARTUP_H
