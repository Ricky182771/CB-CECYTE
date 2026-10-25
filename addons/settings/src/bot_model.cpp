#include "chatbot/addon_settings/bot_model.h"

#include "chatbot/addon_settings/secrets.h"

#include "chatbot/error.h"

#include <optional>

namespace chatbot::addon_settings {

Result<Config> resolve_bot_model(const std::string& config_path,
                                 const std::string& credentials_path,
                                 const Credentials& credentials, std::string_view section_model,
                                 const EnvLookup& env) {
    ConfigOptions options;
    options.env = [](std::string_view) { return std::string_view{}; };
    options.path = [config_path] { return std::optional<std::string>{config_path}; };
    options.credentials_path = [credentials_path] {
        return std::optional<std::string>{credentials_path};
    };
    options.allow_missing_key_and_model = true;
    Result<Config> loaded = load_config(options);
    if (loaded.is_error()) {
        return loaded.error();
    }
    Config config = loaded.value();
    if (!section_model.empty()) {
        config.model = std::string{section_model};
    }
    config.api_key = resolve_model_key(env, credentials, config.api_key);
    if (config.api_key.empty() && !is_local_base_url(config.base_url)) {
        return ChatError{ErrorKind::Config, 0, std::string{kMissingModelKey}, std::nullopt};
    }
    if (config.model.empty()) {
        return ChatError{ErrorKind::Config, 0, std::string{kMissingModel}, std::nullopt};
    }
    if (std::optional<ChatError> error = validate_config(config)) {
        return *error;
    }
    return config;
}

} // namespace chatbot::addon_settings
