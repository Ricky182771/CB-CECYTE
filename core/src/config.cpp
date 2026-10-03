#include "chatbot/config.h"

#include "config_internal.h"

#include <nlohmann/json.hpp>

#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace chatbot {
namespace {

using nlohmann::json;

const char* kDefaultBaseUrl = "https://integrate.api.nvidia.com/v1";

/// Timeout máximo aceptado (1 hora). Evita desbordar la conversión a
/// milisegundos con valores enormes.
constexpr std::chrono::seconds kMaxTimeout{3600};

/// Texto del máximo para los mensajes de error.
std::string max_timeout_text() {
    return std::to_string(kMaxTimeout.count()) + " segundos";
}

/// Lee el entorno real del proceso. Devuelve vacío si no existe la variable.
std::string_view real_env(std::string_view name) {
    // Los nombres que pasamos son literales, así que la conversión a cadena
    // terminada en nulo es segura.
    const char* value = ::getenv(std::string{name}.c_str());
    return value != nullptr ? std::string_view{value} : std::string_view{};
}

/// Lee el archivo completo. Nullopt si no existe o no se puede abrir/leer:
/// que el archivo no exista no es un error (sección 7).
std::optional<std::string> read_file(const std::string& path) {
    std::ifstream file{path, std::ios::binary};
    if (!file.is_open()) {
        return std::nullopt;
    }
    std::string content{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
    if (file.bad()) {
        return std::nullopt;
    }
    return content;
}

/// Entero no negativo desde texto. Nullopt si no es un entero válido.
std::optional<long long> parse_nonnegative_integer(std::string_view text) {
    const std::string terminated{text};
    char* end = nullptr;
    errno = 0;
    const long long value = std::strtoll(terminated.c_str(), &end, 10);
    if (errno != 0 || end != terminated.c_str() + terminated.size() || value < 0) {
        return std::nullopt;
    }
    return value;
}

/// Indica si la URL empieza con https:// sin distinguir mayúsculas.
bool starts_with_https(std::string_view url) {
    constexpr std::string_view kScheme = "https://";
    if (url.size() < kScheme.size()) {
        return false;
    }
    for (std::size_t i = 0; i < kScheme.size(); ++i) {
        const auto c = static_cast<unsigned char>(url[i]);
        if (std::tolower(c) != kScheme[i]) {
            return false;
        }
    }
    return true;
}

/// Aplica al config los valores presentes en el archivo JSON.
/// Lanza std::runtime_error ante JSON malformado o campos de tipo incorrecto
/// (las excepciones se capturan en load_config y no cruzan la API pública).
void apply_config_file(const std::string& path, Config& config) {
    const std::optional<std::string> content = read_file(path);
    if (!content.has_value()) {
        return; // Sin archivo no hay nada que aplicar, y no es un error.
    }

    const json document = json::parse(*content);
    if (!document.is_object()) {
        throw std::runtime_error("el contenido debe ser un objeto JSON");
    }

    // Solo se reconocen las llaves de la sección 7; el resto se ignora.
    if (document.contains("base_url")) {
        const json& value = document.at("base_url");
        if (!value.is_string()) {
            throw std::runtime_error("\"base_url\" debe ser una cadena");
        }
        config.base_url = value.get<std::string>();
    }
    if (document.contains("model")) {
        const json& value = document.at("model");
        if (!value.is_string()) {
            throw std::runtime_error("\"model\" debe ser una cadena");
        }
        config.model = value.get<std::string>();
    }
    if (document.contains("timeout_seconds")) {
        const json& value = document.at("timeout_seconds");
        if (!value.is_number_integer()) {
            throw std::runtime_error("\"timeout_seconds\" debe ser un número entero");
        }
        // Los enteros positivos se guardan sin signo: se comparan así para no
        // convertir a long long un valor que no cabe.
        if (value.is_number_unsigned()) {
            if (value.get<std::uint64_t>() >
                static_cast<std::uint64_t>(kMaxTimeout.count())) {
                throw std::runtime_error("\"timeout_seconds\" no puede ser mayor que " +
                                         max_timeout_text());
            }
        } else if (value.get<long long>() < 0) {
            throw std::runtime_error("\"timeout_seconds\" no puede ser negativo");
        }
        config.timeout_seconds = std::chrono::seconds{value.get<long long>()};
    }
}

} // namespace

std::optional<std::string> build_config_path(
    const std::optional<std::string>& xdg_config_home,
    const std::optional<std::string>& home) {
    if (xdg_config_home.has_value() && !xdg_config_home->empty()) {
        std::string base = *xdg_config_home;
        if (base.back() == '/') {
            base.pop_back();
        }
        return base + "/chatbot/config.json";
    }
    if (home.has_value() && !home->empty()) {
        return *home + "/.config/chatbot/config.json";
    }
    return std::nullopt;
}

std::optional<std::string> default_config_path() {
    const char* xdg = ::getenv("XDG_CONFIG_HOME");
    const char* home = ::getenv("HOME");
    const std::optional<std::string> xdg_value =
        xdg != nullptr ? std::optional<std::string>{std::string{xdg}} : std::nullopt;
    const std::optional<std::string> home_value =
        home != nullptr ? std::optional<std::string>{std::string{home}} : std::nullopt;
    return build_config_path(xdg_value, home_value);
}

std::optional<ChatError> validate_config(const Config& config) {
    if (config.api_key.empty()) {
        return ChatError{
            ErrorKind::Config, 0,
            "Falta la API key: defínela en la variable de entorno CHAT_API_KEY "
            "(por seguridad, la key nunca se lee del archivo de configuración).", std::nullopt};
    }
    if (config.model.empty()) {
        return ChatError{
            ErrorKind::Config, 0,
            "Falta el modelo: defínelo en la variable de entorno CHAT_MODEL o "
            "en la llave \"model\" del archivo de configuración.", std::nullopt};
    }
    if (!starts_with_https(config.base_url)) {
        // Sin TLS la key viajaría en texto plano (sección 9).
        return ChatError{
            ErrorKind::Config, 0,
            "La URL base debe empezar con https:// (CHAT_BASE_URL o \"base_url\" del "
            "archivo de configuración); se recibió \"" + config.base_url + "\".",
            std::nullopt};
    }
    if (config.timeout_seconds > kMaxTimeout) {
        return ChatError{ErrorKind::Config, 0,
                         "El timeout no puede ser mayor que " + max_timeout_text() +
                             "; se recibió " +
                             std::to_string(config.timeout_seconds.count()) + ".",
                         std::nullopt};
    }
    return std::nullopt;
}

Result<Config> load_config(const ConfigOptions& options) {
    const EnvLookup env = options.env ? options.env : EnvLookup{real_env};
    const PathProvider path = options.path ? options.path : PathProvider{default_config_path};

    Config config;
    config.base_url = kDefaultBaseUrl;

    // 1) Archivo de configuración: la precedencia más baja (sección 7).
    if (const std::optional<std::string> config_path = path()) {
        try {
            apply_config_file(*config_path, config);
        } catch (const std::exception& e) {
            return ChatError{ErrorKind::Config, 0,
                             "Archivo de configuración inválido (" + *config_path + "): " + e.what(), std::nullopt};
        }
    }

    // 2) Variables de entorno: pisan lo que venga del archivo.
    if (const std::string_view value = env("CHAT_API_KEY"); !value.empty()) {
        config.api_key = std::string{value};
    }
    if (const std::string_view value = env("CHAT_BASE_URL"); !value.empty()) {
        config.base_url = std::string{value};
    }
    if (const std::string_view value = env("CHAT_MODEL"); !value.empty()) {
        config.model = std::string{value};
    }
    if (const std::string_view value = env("CHAT_TIMEOUT"); !value.empty()) {
        const std::optional<long long> seconds = parse_nonnegative_integer(value);
        if (!seconds.has_value()) {
            return ChatError{ErrorKind::Config, 0,
                             "CHAT_TIMEOUT debe ser un número entero de segundos; se recibió \"" +
                                 std::string{value} + "\".", std::nullopt};
        }
        if (*seconds > kMaxTimeout.count()) {
            return ChatError{ErrorKind::Config, 0,
                             "CHAT_TIMEOUT no puede ser mayor que " + max_timeout_text() +
                                 "; se recibió \"" + std::string{value} + "\".",
                             std::nullopt};
        }
        config.timeout_seconds = std::chrono::seconds{*seconds};
    }

    // 3) Validación final: key y modelo son obligatorios (sección 7).
    if (const std::optional<ChatError> error = validate_config(config)) {
        return *error;
    }
    return config;
}

} // namespace chatbot
