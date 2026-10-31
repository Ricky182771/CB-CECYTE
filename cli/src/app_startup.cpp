#include "app_startup.h"

#include "addon_launcher.h"
#include "child_process.h"
#include "clipboard.h"
#include "conversation_store.h"
#include "downloads.h"

#include "chatbot/addon_settings/paths.h"
#include "chatbot/addon_settings/registry.h"
#include "chatbot/addon_settings/secrets.h"
#include "chatbot/chat_client.h"
#include "chatbot/curl_transport.h"
#include "chatbot/platform.h"
#include "chatbot/tavily_search.h"

#include <cstdlib>
#include <ctime>
#include <filesystem>
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

AddonsEnvironment addons_environment(ServiceWorker::Post post, std::string chatbot_path) {
    namespace settings = addon_settings;
    const auto absolute = [](std::string_view name) {
        const std::string_view value = process_env(name);
        return value.starts_with('/') ? std::optional<std::string>{value} : std::nullopt;
    };
    AddonsEnvironment environment;
    environment.post = std::move(post);
    environment.run = [](const std::vector<std::string>& argv, const ProcessOptions& options) {
        return run_process(argv, options);
    };
    environment.portable = process_portable_data_dir().has_value();
    // Junto a chatbot, que es quien hace exec del addon (la TUI: executable_dir).
    environment.locate = [dir = std::filesystem::path{chatbot_path}.parent_path().string(),
                          portable = environment.portable](const settings::AddonInfo& addon) {
        if (current_os() == Os::Windows) {
            return std::optional<std::string>{};
        }
        return find_addon_executable(addon, dir, portable, is_executable_file, find_in_path);
    };
    environment.config_path = default_config_path().value_or("");
    environment.credentials_path = default_credentials_path().value_or("");
    const settings::AddonInfo& telegram = *settings::find_addon("telegram");
    environment.data_dir = settings::default_addon_data_dir(telegram.name);
    environment.unit_path = settings::default_unit_path(telegram.unit);
    environment.uid = current_user_id();
    environment.chatbot_path = std::move(chatbot_path);
    environment.xdg_config_home = absolute("XDG_CONFIG_HOME");
    environment.xdg_data_home = absolute("XDG_DATA_HOME");
    for (const std::string_view name :
         {settings::kTokenEnv, settings::kApiKeyEnv, settings::kSearchKeyEnv}) {
        if (!process_env(name).empty()) {
            environment.env_defined.push_back(name);
        }
    }
    environment.home = std::string{process_env("HOME")};
    return environment;
}

std::string sibling_chatbot_path(const std::string& executable) {
    return (std::filesystem::path{executable}.parent_path() / "chatbot").string();
}

} // namespace chatbot::cli
