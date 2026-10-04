#include "chatbot/credentials.h"

#include "atomic_file.h"

#include <nlohmann/json.hpp>

#include <sys/stat.h>

#include <cerrno>
#include <fstream>
#include <sstream>

namespace chatbot {

namespace {

constexpr int kCredentialsVersion = 1;

ChatError config_error(std::string message) {
    return ChatError{ErrorKind::Config, 0, std::move(message), std::nullopt};
}

} // namespace

std::string credentials_key(std::string_view provider, std::string_view base_url) {
    if (provider != "custom") {
        return std::string{provider};
    }
    std::string base{base_url};
    while (!base.empty() && base.back() == '/') {
        base.pop_back();
    }
    return "custom:" + base;
}

Result<Credentials> load_credentials(const std::string& path) {
    struct stat info {};
    if (::stat(path.c_str(), &info) != 0) {
        if (errno == ENOENT) {
            return Credentials{}; // Sin archivo: no hay keys guardadas.
        }
        return config_error("No se pudo leer " + path + ".");
    }
    if ((info.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
        return config_error("credentials.json tiene permisos demasiado abiertos; corre chmod 600 " +
                            path);
    }
    try {
        std::ifstream file{path, std::ios::binary};
        if (!file) {
            return config_error("No se pudo leer " + path + ".");
        }
        std::ostringstream content;
        content << file.rdbuf();
        const nlohmann::json document = nlohmann::json::parse(content.str());
        const auto version = document.find("version");
        if (!document.is_object() || version == document.end() ||
            !version->is_number_integer() || version->get<long long>() != kCredentialsVersion) {
            return config_error(path + " no es un archivo de credenciales de la versión 1.");
        }
        Credentials credentials;
        const auto keys = document.find("keys");
        if (keys != document.end()) {
            if (!keys->is_object()) {
                return config_error(path + ": \"keys\" debe ser un objeto.");
            }
            for (const auto& [name, value] : keys->items()) {
                if (!value.is_string()) {
                    // Sin el valor en el mensaje: podría ser una key.
                    return config_error(path + ": la key de \"" + name + "\" debe ser una cadena.");
                }
                credentials.keys[name] = value.get<std::string>();
            }
        }
        return credentials;
    } catch (const std::exception&) {
        // El texto de nlohmann puede citar el contenido: no se muestra.
        return config_error(path + " no es JSON válido.");
    }
}

std::optional<ChatError> save_credentials(const std::string& path,
                                          const Credentials& credentials) {
    nlohmann::ordered_json document;
    document["version"] = kCredentialsVersion;
    document["keys"] = nlohmann::ordered_json::object();
    for (const auto& [name, key] : credentials.keys) {
        document["keys"][name] = key;
    }
    const std::string content =
        document.dump(4, ' ', false, nlohmann::ordered_json::error_handler_t::replace) + "\n";
    if (const std::optional<std::string> error = write_file_atomic(path, content, 0600, 0700)) {
        return config_error("No se pudieron guardar las credenciales: " + *error);
    }
    return std::nullopt;
}

} // namespace chatbot
