#include "search_settings.h"

#include "provider_settings.h"

#include "chatbot/config.h"

namespace chatbot::cli {

SearchSettings::SearchSettings(std::string saved, std::optional<std::string> env)
    : saved_(std::move(saved)), env_(std::move(env)) {}

bool SearchSettings::set_key(std::string key) {
    if (key_locked()) {
        return false;
    }
    key_ = std::move(key);
    return true;
}

std::string SearchSettings::key_status() const {
    if (env_.has_value()) {
        return "(definido por CHAT_SEARCH_API_KEY) " + mask_key(*env_);
    }
    if (!key_.empty()) {
        return {};
    }
    if (!saved_.empty()) {
        return "guardada: " + mask_key(saved_);
    }
    return "sin configurar";
}

std::optional<std::string> SearchSettings::validate() const {
    if (!key_locked() && !key_.empty() && !plausible_key(key_)) {
        return "Eso parece el nombre de una variable o una key de búsqueda incompleta; pega la "
               "key completa.";
    }
    return std::nullopt;
}

std::optional<std::pair<std::string, std::string>> SearchSettings::credential_update() const {
    if (key_locked() || !plausible_key(key_)) {
        return std::nullopt;
    }
    return std::pair{std::string{kSearchCredentialsKey}, key_};
}

void SearchSettings::mark_saved() {
    if (credential_update().has_value()) {
        saved_ = key_;
    }
    key_.clear();
}

} // namespace chatbot::cli
