#ifndef CHATBOT_CONFIG_H
#define CHATBOT_CONFIG_H

#include "chatbot/result.h"

#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace chatbot {

/// Configuración efectiva del núcleo (sección 7).
struct Config {
    /// CHAT_API_KEY o, si no está, credentials.json (la key del proveedor del
    /// archivo de configuración). Nunca de config.json. Puede quedar vacía
    /// para un servidor local (localhost, 127.0.0.1 o [::1]).
    std::string api_key;
    std::string base_url; ///< Por defecto: https://integrate.api.nvidia.com/v1
    std::string provider; ///< "provider" de config.json (id de la tabla de cli/), o vacío.
    std::string model;    ///< Sin valor por defecto a propósito (sección 7).
    std::chrono::seconds timeout_seconds{120};
    /// Límite del historial que se envía, en bytes UTF-8 de los content
    /// (aproximadamente caracteres). 0 significa sin límite.
    std::size_t history_limit_bytes{32000};
    /// Archivo para el volcado de depuración (solo CHAT_DEBUG_SSE, nunca del
    /// archivo de configuración). Si está, ChatClient agrega por cada intento
    /// el estado HTTP y el cuerpo crudo de la respuesta; nunca cabeceras, key
    /// ni cuerpo de la petición. Contiene la conversación: solo para
    /// diagnosticar.
    std::optional<std::string> debug_sse_path;
};

/// Fuente de variables de entorno, inyectable para las pruebas.
/// Devuelve la cadena vacía si la variable no existe.
using EnvLookup = std::function<std::string_view(std::string_view name)>;

/// Devuelve la ruta del archivo de configuración, o nullopt si no aplica.
using PathProvider = std::function<std::optional<std::string>()>;

/// Opciones de carga, inyectables para las pruebas.
struct ConfigOptions {
    EnvLookup env;     ///< Por defecto lee el entorno real del proceso.
    PathProvider path; ///< Por defecto usa default_config_path().
    /// Por defecto, credentials.json en la misma carpeta que config.json.
    PathProvider credentials_path{};
    /// true: falta de key o de modelo no es error (el resto de la validación
    /// sí se aplica). Lo usa la interfaz para arrancar y abrir la pantalla de
    /// configuración; quien llama revisa después con validate_config.
    bool allow_missing_key_and_model = false;
};

/// Devuelve la ruta del archivo de configuración del entorno real:
/// $XDG_CONFIG_HOME/chatbot/config.json, o ~/.config/chatbot/config.json
/// si XDG_CONFIG_HOME no está. Nullopt si no se puede determinar.
[[nodiscard]] std::optional<std::string> default_config_path();

/// Ruta de credentials.json en el entorno real: la misma carpeta que
/// default_config_path(). Nullopt si no se puede determinar.
[[nodiscard]] std::optional<std::string> default_credentials_path();

/// Carga la configuración con precedencia entorno > archivo > defectos.
/// - La key: CHAT_API_KEY; si no, la de credentials.json para el "provider"
///   de config.json (credentials_key); si no, ninguna. Nunca de config.json.
/// - Que el archivo no exista no es un error.
/// - Falta de key o de modelo produce un error Config (sección 7).
/// Las excepciones internas nunca cruzan esta función (sección 6).
[[nodiscard]] Result<Config> load_config(const ConfigOptions& options = {});

/// Verifica que la configuración sea utilizable: URL base válida (ver
/// validate_base_url), key presente salvo para un servidor local, modelo
/// presente y timeout dentro del máximo. Devuelve el error Config, o nullopt.
[[nodiscard]] std::optional<ChatError> validate_config(const Config& config);

/// Verifica la URL base analizándola (esquema, host y puerto): https://
/// con cualquier host, o http:// solo si el host es exactamente localhost,
/// 127.0.0.1 o [::1] (con cualquier puerto). Sin usuario en la URL. Devuelve
/// el error Config con un mensaje en español, o nullopt.
[[nodiscard]] std::optional<ChatError> validate_base_url(std::string_view url);

/// true si la URL es válida y su host es localhost, 127.0.0.1 o [::1]: ahí
/// la key es opcional.
[[nodiscard]] bool is_local_base_url(std::string_view url);

/// Valores que la pantalla de configuración guarda en config.json.
struct ConfigFileValues {
    std::string provider;
    std::string base_url; ///< Siempre se guarda, también para proveedores conocidos.
    std::string model;
};

/// Lee provider, base_url y model tal como están en config.json, sin el
/// entorno ni valores por defecto (vacíos si faltan). Que el archivo no
/// exista no es un error; si no es un objeto JSON válido, error Config.
[[nodiscard]] Result<ConfigFileValues> load_config_file_values(const std::string& path);

/// Escribe provider, base_url y model en config.json de forma atómica,
/// conservando todas las demás llaves que ya tuviera el archivo (y su
/// orden). Nunca escribe una key. Si el archivo existe pero no es un objeto
/// JSON válido, no lo toca y devuelve el error Config.
[[nodiscard]] std::optional<ChatError> save_config_file(const std::string& path,
                                                        const ConfigFileValues& values);

} // namespace chatbot

#endif // CHATBOT_CONFIG_H
