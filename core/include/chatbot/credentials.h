#ifndef CHATBOT_CREDENTIALS_H
#define CHATBOT_CREDENTIALS_H

#include "chatbot/error.h"
#include "chatbot/result.h"

#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace chatbot {

/// API keys guardadas por el usuario (credentials.json, sección 7): una por
/// proveedor; para "custom", una por URL base. Nunca se escriben en
/// config.json, logs, volcados ni mensajes de error (sección 9).
struct Credentials {
    std::map<std::string, std::string> keys; ///< Llave (ver credentials_key) → API key.
};

/// Llave de credentials.json: el id del proveedor, o "custom:" + la URL base
/// sin "/" final para un endpoint personalizado.
[[nodiscard]] std::string credentials_key(std::string_view provider, std::string_view base_url);

/// Lee credentials.json. Que no exista no es un error (sin keys). Si grupo u
/// otros tienen algún permiso sobre el archivo, no se lee y se devuelve un
/// error Config (el criterio de ssh con sus llaves). JSON inválido o de otra
/// versión también es error Config. El mensaje nunca incluye las keys.
[[nodiscard]] Result<Credentials> load_credentials(const std::string& path);

/// Escribe credentials.json de forma atómica, en 0600, con la carpeta en
/// 0700. Devuelve el error Config, o nullopt.
[[nodiscard]] std::optional<ChatError> save_credentials(const std::string& path,
                                                        const Credentials& credentials);

} // namespace chatbot

#endif // CHATBOT_CREDENTIALS_H
