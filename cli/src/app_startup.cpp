#include "app_startup.h"

#include "clipboard.h"
#include "conversation_store.h"
#include "downloads.h"

#include "chatbot/chat_client.h"
#include "chatbot/curl_transport.h"
#include "chatbot/platform.h"
#include "chatbot/tavily_search.h"

#include <cstdlib>
#include <ctime>
#include <memory>
#include <optional>
#include <string>

namespace chatbot::cli {

Result<Config> load_startup_config() {
    // Modo portable (chatbot.portable junto al ejecutable): la configuración,
    // la key y las conversaciones van a datos/. Si no se puede escribir ahí,
    // es un error; nunca se usa en silencio la carpeta del usuario.
    const std::optional<std::string>& portable_dir = process_portable_data_dir();
    if (portable_dir.has_value()) {
        if (std::optional<ChatError> error = prepare_portable_data_dir(*portable_dir)) {
            return std::move(*error);
        }
    }
    ConfigOptions options;
    options.allow_missing_key_and_model = true;
    return load_config(options);
}

std::string_view process_env(std::string_view name) {
    const char* value = std::getenv(std::string{name}.c_str());
    return value != nullptr ? std::string_view{value} : std::string_view{};
}

SessionEnvironment app_session_environment() {
    SessionEnvironment environment;
    environment.config_path = default_config_path();
    environment.credentials_path = default_credentials_path();
    environment.data_dir = default_data_dir().value_or("");
    environment.env = process_env;
    environment.make_client = [](const Config& complete) {
        return std::make_unique<ChatClient>(complete, std::make_unique<CurlTransport>());
    };
    environment.make_search = [](const std::string& key) -> std::shared_ptr<SearchProvider> {
        return std::make_shared<TavilySearch>(key, std::make_unique<CurlTransport>());
    };
    environment.now = [] { return std::time(nullptr); };
    environment.download_base = [] { return default_download_dir(environment_value); };
    environment.home = display_home(environment_value, current_os());
    return environment;
}

} // namespace chatbot::cli
