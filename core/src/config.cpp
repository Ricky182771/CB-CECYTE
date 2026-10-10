#include "chatbot/config.h"

#include "config_internal.h"

#include "chatbot/credentials.h"
#include "chatbot/platform.h"
#include "url.h"

#include <nlohmann/json.hpp>

#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iterator>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
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

/// true si path existe. Cualquier error al revisarlo cuenta como que no
/// existe, igual que un archivo que falta.
bool file_exists(const std::string& path) {
    try {
        std::error_code error;
        return std::filesystem::exists(std::filesystem::path{path}, error);
    } catch (const std::exception&) {
        return false; // En Windows, fs::path lanza si la ruta no es UTF-8 válido.
    }
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
    if (document.contains("provider")) {
        const json& value = document.at("provider");
        if (!value.is_string()) {
            throw std::runtime_error("\"provider\" debe ser una cadena");
        }
        config.provider = value.get<std::string>();
    }
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
    if (document.contains("history_limit")) {
        const json& value = document.at("history_limit");
        if (!value.is_number_integer()) {
            throw std::runtime_error("\"history_limit\" debe ser un número entero de bytes");
        }
        if (!value.is_number_unsigned()) {
            // Un entero con signo que llega del parser solo puede ser negativo.
            if (value.get<long long>() < 0) {
                throw std::runtime_error("\"history_limit\" no puede ser negativo");
            }
        }
        const std::uint64_t bytes = value.get<std::uint64_t>();
        if (bytes > std::numeric_limits<std::size_t>::max()) {
            throw std::runtime_error("\"history_limit\" es demasiado grande");
        }
        config.history_limit_bytes = static_cast<std::size_t>(bytes);
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

std::optional<std::string> build_windows_config_path(
    const std::optional<std::string>& roaming_app_data,
    const std::optional<std::string>& appdata) {
    for (const std::optional<std::string>* base : {&roaming_app_data, &appdata}) {
        if (base->has_value() && !(*base)->empty()) {
            return join_windows_path(**base, "chatbot\\config.json");
        }
    }
    return std::nullopt;
}

std::string missing_config_dir_message(Os os) {
    if (os == Os::Windows) {
        return "No se encontró la carpeta de configuración: Windows no dio la carpeta "
               "AppData\\Roaming y la variable APPDATA no está definida.";
    }
    return "No se encontró la carpeta de configuración (define HOME o XDG_CONFIG_HOME).";
}

std::optional<std::string> default_config_path() {
    if (current_os() == Os::Windows) {
        const char* appdata = ::getenv("APPDATA");
        return build_windows_config_path(
            known_folder(KnownFolder::RoamingAppData),
            appdata != nullptr ? std::optional<std::string>{std::string{appdata}}
                               : std::nullopt);
    }
    const char* xdg = ::getenv("XDG_CONFIG_HOME");
    const char* home = ::getenv("HOME");
    const std::optional<std::string> xdg_value =
        xdg != nullptr ? std::optional<std::string>{std::string{xdg}} : std::nullopt;
    const std::optional<std::string> home_value =
        home != nullptr ? std::optional<std::string>{std::string{home}} : std::nullopt;
    return build_config_path(xdg_value, home_value);
}

std::optional<std::string> default_credentials_path() {
    const std::optional<std::string> config_path = default_config_path();
    if (!config_path.has_value()) {
        return std::nullopt;
    }
    return (std::filesystem::path{*config_path}.parent_path() / "credentials.json").string();
}

std::optional<ChatError> validate_base_url(std::string_view url) {
    const std::optional<ParsedUrl> parsed = parse_url(url);
    const auto invalid = [url](const std::string& why) {
        return ChatError{ErrorKind::Config, 0,
                         "La URL base no es válida (CHAT_BASE_URL o \"base_url\" del archivo de "
                         "configuración): " + why + "; se recibió \"" + std::string{url} + "\".",
                         std::nullopt};
    };
    if (!parsed.has_value()) {
        return invalid("debe tener la forma https://servidor/ruta, sin usuario ni contraseña");
    }
    if (parsed->scheme == "https") {
        return std::nullopt;
    }
    if (parsed->scheme == "http" && is_local_host(parsed->host)) {
        // Sin TLS solo en la propia máquina: la key no sale de ella.
        return std::nullopt;
    }
    if (parsed->scheme == "http") {
        // Sin TLS la key viajaría en texto plano por la red (sección 9).
        return invalid("debe empezar con https://; http:// solo se acepta para localhost, "
                       "127.0.0.1 o [::1]");
    }
    return invalid("el esquema debe ser https:// (o http:// en la propia máquina)");
}

bool is_local_base_url(std::string_view url) {
    const std::optional<ParsedUrl> parsed = parse_url(url);
    return parsed.has_value() && !validate_base_url(url).has_value() &&
           is_local_host(parsed->host);
}

bool base_url_has_path(std::string_view url) {
    const std::optional<ParsedUrl> parsed = parse_url(url);
    // Solo barras ("/", "//") no cuentan: build_url las quita.
    return parsed.has_value() && !validate_base_url(url).has_value() &&
           parsed->path.find_first_not_of('/') != std::string::npos;
}

namespace {

/// URL base y timeout: lo que se valida aunque falten la key o el modelo.
std::optional<ChatError> validate_url_and_timeout(const Config& config) {
    if (std::optional<ChatError> error = validate_base_url(config.base_url)) {
        return error;
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

} // namespace

std::optional<ChatError> validate_connection(const Config& config) {
    if (std::optional<ChatError> error = validate_url_and_timeout(config)) {
        return error;
    }
    if (config.api_key.empty() && !is_local_base_url(config.base_url)) {
        return ChatError{
            ErrorKind::Config, 0,
            "Falta la API key: defínela en la variable de entorno CHAT_API_KEY o guárdala en "
            "la configuración (credentials.json; nunca en config.json).", std::nullopt};
    }
    return std::nullopt;
}

std::optional<ChatError> validate_config(const Config& config) {
    if (std::optional<ChatError> error = validate_connection(config)) {
        return error;
    }
    if (config.model.empty()) {
        return ChatError{
            ErrorKind::Config, 0,
            "Falta el modelo: defínelo en la variable de entorno CHAT_MODEL o "
            "en la llave \"model\" del archivo de configuración.", std::nullopt};
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
    if (const std::string_view value = env("CHAT_DEBUG_SSE"); !value.empty()) {
        config.debug_sse_path = std::string{value};
    }
    if (const std::string_view value = env("CHAT_HISTORY_LIMIT"); !value.empty()) {
        const std::optional<long long> bytes = parse_nonnegative_integer(value);
        if (!bytes.has_value()) {
            return ChatError{ErrorKind::Config, 0,
                             "CHAT_HISTORY_LIMIT debe ser un número entero no negativo de "
                             "bytes (0 = sin límite); se recibió \"" +
                                 std::string{value} + "\".",
                             std::nullopt};
        }
        config.history_limit_bytes = static_cast<std::size_t>(*bytes);
    }

    // 3) Sin CHAT_API_KEY: la key guardada para el proveedor de config.json.
    if (config.api_key.empty() && !config.provider.empty()) {
        std::optional<std::string> credentials_path;
        if (options.credentials_path) {
            credentials_path = options.credentials_path();
        } else if (const std::optional<std::string> config_path = path()) {
            credentials_path =
                (std::filesystem::path{*config_path}.parent_path() / "credentials.json").string();
        }
        if (credentials_path.has_value()) {
            Result<Credentials> credentials = load_credentials(*credentials_path);
            if (credentials.is_error()) {
                return credentials.error();
            }
            const auto key = credentials.value().keys.find(
                credentials_key(config.provider, config.base_url));
            if (key != credentials.value().keys.end()) {
                config.api_key = key->second;
            }
        }
    }

    // 4) Validación final: URL, key (salvo local) y modelo (sección 7).
    if (options.allow_missing_key_and_model) {
        if (const std::optional<ChatError> error = validate_url_and_timeout(config)) {
            return *error;
        }
        return config;
    }
    if (const std::optional<ChatError> error = validate_config(config)) {
        return *error;
    }
    return config;
}

Result<ConfigFileValues> load_config_file_values(const std::string& path) {
    ConfigFileValues values;
    if (!file_exists(path)) {
        return values; // Sin archivo: todo vacío.
    }
    try {
        const std::optional<std::string> content = read_file(path);
        if (!content.has_value()) {
            throw std::runtime_error("no se pudo leer");
        }
        const nlohmann::json document = nlohmann::json::parse(*content);
        if (!document.is_object()) {
            throw std::runtime_error("no es un objeto JSON");
        }
        const auto field = [&document](const char* name) {
            const auto it = document.find(name);
            return it != document.end() && it->is_string() ? it->get<std::string>()
                                                           : std::string{};
        };
        values.provider = field("provider");
        values.base_url = field("base_url");
        values.model = field("model");
    } catch (const std::exception&) {
        return ChatError{ErrorKind::Config, 0,
                         "Archivo de configuración inválido (" + path + "): no es un objeto "
                         "JSON válido.", std::nullopt};
    }
    return values;
}

namespace {

using ordered = nlohmann::ordered_json;

/// Lee config.json (o un objeto vacío si no existe), deja que change lo
/// modifique y lo escribe de forma atómica, conservando las demás llaves, su
/// orden y los permisos del archivo. Si existe pero no es un objeto JSON
/// válido, no lo toca y devuelve el error Config.
std::optional<ChatError> update_config_file(const std::string& path,
                                            const std::function<void(ordered&)>& change) {
    ordered document = ordered::object();
    if (file_exists(path)) {
        const std::optional<std::string> content = read_file(path);
        try {
            if (!content.has_value()) {
                throw std::runtime_error("no se pudo leer");
            }
            document = ordered::parse(*content);
            if (!document.is_object()) {
                throw std::runtime_error("no es un objeto JSON");
            }
        } catch (const std::exception&) {
            return ChatError{ErrorKind::Config, 0,
                             "No se guardó la configuración: " + path +
                                 " no es un objeto JSON válido (corrígelo o bórralo).",
                             std::nullopt};
        }
    }
    change(document);
    const std::string text =
        document.dump(4, ' ', false, ordered::error_handler_t::replace) + "\n";
    // config.json no lleva secretos: conserva los permisos que tenía, o 0644.
    if (const std::optional<std::string> error =
            write_file_atomic(path, text, FilePrivacy::KeepExisting, FolderPrivacy::Private)) {
        return ChatError{ErrorKind::Config, 0, "No se guardó la configuración: " + *error,
                         std::nullopt};
    }
    return std::nullopt;
}

} // namespace

std::optional<ChatError> save_config_file(const std::string& path,
                                          const ConfigFileValues& values) {
    return update_config_file(path, [&values](ordered& document) {
        document["provider"] = values.provider;
        document["base_url"] = values.base_url;
        document["model"] = values.model;
    });
}

Result<AppearanceValues> load_appearance_values(const std::string& path) {
    AppearanceValues values;
    if (!file_exists(path)) {
        return values; // Sin archivo: todo vacío.
    }
    try {
        const std::optional<std::string> content = read_file(path);
        if (!content.has_value()) {
            throw std::runtime_error("no se pudo leer");
        }
        const nlohmann::json document = nlohmann::json::parse(*content);
        if (!document.is_object()) {
            throw std::runtime_error("no es un objeto JSON");
        }
        const auto appearance = document.find("appearance");
        if (appearance == document.end() || !appearance->is_object()) {
            return values;
        }
        const auto field = [&appearance](const char* name) {
            const auto it = appearance->find(name);
            return it != appearance->end() && it->is_string() ? it->get<std::string>()
                                                              : std::string{};
        };
        values.theme = field("theme");
        values.background = field("background");
    } catch (const std::exception&) {
        return ChatError{ErrorKind::Config, 0,
                         "Archivo de configuración inválido (" + path + "): no es un objeto "
                         "JSON válido.", std::nullopt};
    }
    return values;
}

std::optional<ChatError> save_appearance(const std::string& path,
                                         const AppearanceValues& values) {
    return update_config_file(path, [&values](ordered& document) {
        ordered& appearance = document["appearance"];
        if (!appearance.is_object()) {
            appearance = ordered::object();
        }
        appearance["theme"] = values.theme;
        appearance["background"] = values.background;
    });
}

Result<std::optional<std::string>> load_system_prompt_value(const std::string& path) {
    if (!file_exists(path)) {
        return std::optional<std::string>{}; // Sin archivo: la llave falta.
    }
    nlohmann::json document;
    try {
        const std::optional<std::string> content = read_file(path);
        if (!content.has_value()) {
            throw std::runtime_error("no se pudo leer");
        }
        document = nlohmann::json::parse(*content);
        if (!document.is_object()) {
            throw std::runtime_error("no es un objeto JSON");
        }
    } catch (const std::exception&) {
        return ChatError{ErrorKind::Config, 0,
                         "Archivo de configuración inválido (" + path + "): no es un objeto "
                         "JSON válido.", std::nullopt};
    }
    const auto prompt = document.find("system_prompt");
    if (prompt == document.end()) {
        return std::optional<std::string>{};
    }
    if (!prompt->is_string()) {
        return ChatError{ErrorKind::Config, 0,
                         "\"system_prompt\" de " + path + " debe ser una cadena.",
                         std::nullopt};
    }
    return std::optional<std::string>{prompt->get<std::string>()};
}

std::optional<ChatError> save_system_prompt(const std::string& path,
                                            const std::optional<std::string>& prompt) {
    return update_config_file(path, [&prompt](ordered& document) {
        if (prompt.has_value()) {
            document["system_prompt"] = *prompt;
        } else {
            document.erase("system_prompt");
        }
    });
}

Result<std::string> load_search_api_key(const EnvLookup& env,
                                        const std::string& credentials_path) {
    // Primero revisa la variable de entorno.
    const std::string_view env_key = env("CHAT_SEARCH_API_KEY");
    if (!env_key.empty()) {
        return std::string{env_key};
    }

    // Si no está, intenta cargar de credentials.json.
    const Result<Credentials> credentials = load_credentials(credentials_path);
    if (credentials.is_error()) {
        // Propagar el error, excepto si el archivo no existe (eso no es un error).
        // load_credentials devuelve error Config solo si el archivo existe con
        // permisos incorrectos o JSON inválido.
        return credentials.error();
    }

    const auto it = credentials.value().keys.find(std::string{kSearchCredentialsKey});
    if (it != credentials.value().keys.end()) {
        return it->second;
    }

    return std::string{};  // No configurada (cadena vacía).
}

} // namespace chatbot
