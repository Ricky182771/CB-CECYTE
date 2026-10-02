#ifndef CHATBOT_CONFIG_H
#define CHATBOT_CONFIG_H

#include "chatbot/result.h"

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace chatbot {

/// Configuración efectiva del núcleo (sección 7).
struct Config {
    std::string api_key;  ///< Viene solo del entorno; nunca del archivo.
    std::string base_url; ///< Por defecto: https://integrate.api.nvidia.com/v1
    std::string model;    ///< Sin valor por defecto a propósito (sección 7).
    std::chrono::seconds timeout_seconds{120};
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
};

/// Devuelve la ruta del archivo de configuración del entorno real:
/// $XDG_CONFIG_HOME/chatbot/config.json, o ~/.config/chatbot/config.json
/// si XDG_CONFIG_HOME no está. Nullopt si no se puede determinar.
[[nodiscard]] std::optional<std::string> default_config_path();

/// Carga la configuración con precedencia entorno > archivo > defectos.
/// - La key se toma únicamente de CHAT_API_KEY.
/// - Que el archivo no exista no es un error.
/// - Falta de key o de modelo produce un error Config (sección 7).
/// Las excepciones internas nunca cruzan esta función (sección 6).
[[nodiscard]] Result<Config> load_config(const ConfigOptions& options = {});

/// Verifica que la configuración sea utilizable (key y modelo presentes).
/// Devuelve el error Config correspondiente, o nullopt si es válida.
[[nodiscard]] std::optional<ChatError> validate_config(const Config& config);

} // namespace chatbot

#endif // CHATBOT_CONFIG_H
