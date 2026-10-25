#include "chatbot/addon_settings/secrets.h"

namespace chatbot::addon_settings {
namespace {

std::string from_env(const EnvLookup& env, std::string_view name) {
    return env ? std::string{env(name)} : std::string{};
}

} // namespace

std::string credential(const Credentials& credentials, std::string_view key) {
    const auto found = credentials.keys.find(std::string{key});
    return found != credentials.keys.end() ? found->second : std::string{};
}

std::string resolve_token(const EnvLookup& env, const Credentials& credentials) {
    std::string token = from_env(env, kTokenEnv);
    return token.empty() ? credential(credentials, kTokenCredentialsKey) : token;
}

std::string resolve_model_key(const EnvLookup& env, const Credentials& credentials,
                              std::string_view app_key) {
    for (std::string key : {from_env(env, kApiKeyEnv),
                            credential(credentials, kBotApiKeyCredentialsKey)}) {
        if (!key.empty()) {
            return key;
        }
    }
    return std::string{app_key};
}

std::string resolve_search_key(const EnvLookup& env, const Credentials& credentials) {
    for (std::string key : {from_env(env, kSearchKeyEnv),
                            credential(credentials, kBotSearchKeyCredentialsKey)}) {
        if (!key.empty()) {
            return key;
        }
    }
    return credential(credentials, kSearchCredentialsKey);
}

} // namespace chatbot::addon_settings
