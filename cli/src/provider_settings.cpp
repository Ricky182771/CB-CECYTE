#include "provider_settings.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string_view>
#include <utility>

namespace chatbot::cli {

namespace {

/// Largo mínimo de una key escrita en el formulario.
constexpr std::size_t kMinKeyLength = 20;

std::string lowercase(std::string_view text) {
    std::string out{text};
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

/// Proveedor de lo guardado: el de "provider"; si no hay, el de la tabla con
/// esa URL base (sin URL: NVIDIA, el valor por defecto del núcleo); si no,
/// "custom".
std::size_t initial_provider(const ConfigFileValues& saved) {
    if (!saved.provider.empty()) {
        return provider_index(saved.provider);
    }
    if (saved.base_url.empty()) {
        return 0;
    }
    const auto table = providers();
    for (std::size_t i = 0; i < table.size(); ++i) {
        if (!table[i].base_url.empty() && table[i].base_url == saved.base_url) {
            return i;
        }
    }
    return provider_index(kCustomProvider);
}

} // namespace

bool plausible_key(std::string_view key) {
    if (key.size() < kMinKeyLength || key.front() == '$') {
        return false;
    }
    return key.find_first_of(" \t\n\r\v\f") == std::string_view::npos;
}

std::string mask_key(std::string_view key) {
    static const std::string kEllipsis = "…";
    if (key.size() < 8) {
        return kEllipsis;
    }
    const std::string_view tail = key.substr(key.size() - 4);
    for (const char c : tail) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte < 0x21 || byte > 0x7e) {
            return kEllipsis;
        }
    }
    return kEllipsis + std::string{tail};
}

ProviderSettings::ProviderSettings(ConfigFileValues saved, Credentials credentials,
                                   SettingsEnv env)
    : saved_(std::move(saved)), credentials_(std::move(credentials)), env_(std::move(env)) {
    saved_provider_ = initial_provider(saved_);
    provider_ = saved_provider_;
    if (provider_info().id == kCustomProvider) {
        custom_url_ = saved_.base_url;
        base_url_ = custom_url_;
    } else {
        base_url_ = std::string{provider_info().base_url};
    }
    model_ = saved_.model;
    initial_provider_ = provider_;
    initial_url_ = base_url_;
    initial_model_ = model_;
}

const ProviderInfo& ProviderSettings::provider_info() const { return providers()[provider_]; }

void ProviderSettings::select_provider(std::size_t index) {
    if (index >= providers().size() || index == provider_) {
        return;
    }
    provider_ = index;
    base_url_ = provider_info().id == kCustomProvider ? custom_url_
                                                       : std::string{provider_info().base_url};
    key_.clear();
    filter_.clear();
    models_.clear();
    models_error_.clear();
    models_state_ = ModelsState::Idle;
    highlighted_ = 0;
    model_ = index == saved_provider_ ? saved_.model : std::string{};
}

std::string ProviderSettings::provider_label() const {
    if (provider_info().id == kCustomProvider) {
        return effective_base_url();
    }
    return std::string{provider_info().name};
}

const std::string& ProviderSettings::effective_base_url() const {
    return env_.base_url.has_value() ? *env_.base_url : base_url_;
}

bool ProviderSettings::base_url_editable() const {
    return provider_info().id == kCustomProvider && !base_url_locked();
}

std::optional<std::string> ProviderSettings::base_url_hint() const {
    const std::string& url = effective_base_url();
    // Hay servidores que sirven en la raíz: es solo un aviso.
    if (provider_info().id != kCustomProvider || validate_base_url(url).has_value() ||
        base_url_has_path(url)) {
        return std::nullopt;
    }
    return "Casi todos los servidores compatibles con OpenAI usan una ruta como /v1 "
           "(p. ej. https://servidor/v1).";
}

bool ProviderSettings::set_base_url(std::string url) {
    if (!base_url_editable()) {
        return false;
    }
    if (url != base_url_) {
        models_state_ = ModelsState::Idle; // La lista era de otra URL.
        models_.clear();
        models_error_.clear();
        highlighted_ = 0;
    }
    base_url_ = std::move(url);
    custom_url_ = base_url_;
    return true;
}

bool ProviderSettings::set_key(std::string key) {
    if (key_locked()) {
        return false;
    }
    key_ = std::move(key);
    return true;
}

std::string ProviderSettings::saved_key() const {
    const auto it = credentials_.keys.find(
        credentials_key(provider_info().id, effective_base_url()));
    return it != credentials_.keys.end() ? it->second : std::string{};
}

std::string ProviderSettings::effective_key() const {
    if (env_.api_key.has_value()) {
        return *env_.api_key;
    }
    return key_.empty() ? saved_key() : key_;
}

std::string ProviderSettings::key_status() const {
    if (env_.api_key.has_value()) {
        return "(definido por CHAT_API_KEY) " + mask_key(*env_.api_key);
    }
    if (!key_.empty()) {
        return {};
    }
    if (const std::string saved = saved_key(); !saved.empty()) {
        return "guardada: " + mask_key(saved);
    }
    if (is_local_base_url(effective_base_url())) {
        return "opcional en local";
    }
    return {};
}

const std::string& ProviderSettings::effective_model() const {
    return env_.model.has_value() ? *env_.model : model_;
}

bool ProviderSettings::set_model(std::string model) {
    if (model_locked()) {
        return false;
    }
    model_ = std::move(model);
    return true;
}

void ProviderSettings::models_loading() {
    models_state_ = ModelsState::Loading;
    models_error_.clear();
}

void ProviderSettings::models_loaded(std::vector<std::string> models) {
    models_state_ = ModelsState::Loaded;
    models_error_.clear();
    models_ = std::move(models);
    highlighted_ = 0;
    // El resaltado empieza en el modelo elegido, si está en la lista.
    const std::vector<std::string> visible = filtered_models();
    const auto it = std::find(visible.begin(), visible.end(), effective_model());
    if (it != visible.end()) {
        highlighted_ = static_cast<std::size_t>(it - visible.begin());
    }
}

void ProviderSettings::models_failed(std::string message) {
    models_state_ = ModelsState::Failed;
    models_error_ = std::move(message);
    models_.clear();
    highlighted_ = 0;
}

void ProviderSettings::set_filter(std::string filter) {
    filter_ = std::move(filter);
    highlighted_ = 0;
}

std::vector<std::string> ProviderSettings::filtered_models() const {
    const std::string needle = lowercase(filter_);
    std::vector<std::string> out;
    for (const std::string& model : models_) {
        if (lowercase(model).find(needle) != std::string::npos) {
            out.push_back(model);
        }
    }
    return out;
}

void ProviderSettings::move_highlight(int delta) {
    const std::size_t count = filtered_models().size();
    if (count == 0) {
        highlighted_ = 0;
        return;
    }
    const long long next = static_cast<long long>(highlighted_) + delta;
    highlighted_ = static_cast<std::size_t>(
        std::clamp<long long>(next, 0, static_cast<long long>(count) - 1));
}

bool ProviderSettings::pick_highlighted() {
    const std::vector<std::string> visible = filtered_models();
    if (visible.empty() || model_locked()) {
        return false;
    }
    model_ = visible[std::min(highlighted_, visible.size() - 1)];
    return true;
}

bool ProviderSettings::can_request_models() const {
    const std::string& url = effective_base_url();
    return !validate_base_url(url).has_value() &&
           (!effective_key().empty() || is_local_base_url(url));
}

Config ProviderSettings::request_config(Config base) const {
    base.provider = std::string{provider_info().id};
    base.base_url = effective_base_url();
    base.api_key = effective_key();
    base.model = effective_model();
    return base;
}

std::optional<std::string> ProviderSettings::validate() const {
    const std::string& url = effective_base_url();
    if (url.empty()) {
        return "Escribe la URL base del servidor.";
    }
    if (const std::optional<ChatError> error = validate_base_url(url)) {
        return error->message;
    }
    // El mensaje nunca incluye la key.
    if (!key_locked() && !key_.empty() && !plausible_key(key_)) {
        return "Eso parece el nombre de una variable o una key incompleta; pega la key completa.";
    }
    if (effective_key().empty() && !is_local_base_url(url)) {
        return "Falta la API key de " + provider_label() + ".";
    }
    if (effective_model().empty()) {
        return "Elige un modelo de la lista o escríbelo.";
    }
    return std::nullopt;
}

ConfigFileValues ProviderSettings::file_values() const {
    ConfigFileValues values;
    values.provider = std::string{provider_info().id};
    values.base_url = base_url_;
    values.model = model_locked() ? saved_.model : model_;
    return values;
}

std::optional<std::pair<std::string, std::string>> ProviderSettings::credential_update() const {
    if (key_locked() || !plausible_key(key_)) {
        return std::nullopt;
    }
    return std::pair{credentials_key(provider_info().id, effective_base_url()), key_};
}

bool ProviderSettings::dirty() const {
    return provider_ != initial_provider_ || base_url_ != initial_url_ ||
           model_ != initial_model_ || !key_.empty();
}

void ProviderSettings::mark_saved() {
    if (const auto update = credential_update()) {
        credentials_.keys[update->first] = update->second;
    }
    saved_ = file_values();
    saved_provider_ = provider_;
    key_.clear();
    initial_provider_ = provider_;
    initial_url_ = base_url_;
    initial_model_ = model_;
}

} // namespace chatbot::cli
