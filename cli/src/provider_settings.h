#ifndef CHATBOT_CLI_PROVIDER_SETTINGS_H
#define CHATBOT_CLI_PROVIDER_SETTINGS_H

#include "providers.h"

#include "chatbot/config.h"
#include "chatbot/credentials.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace chatbot::cli {

/// Variables CHAT_* que pisan el formulario (solo las definidas y no vacías).
struct SettingsEnv {
    std::optional<std::string> base_url; ///< CHAT_BASE_URL
    std::optional<std::string> model;    ///< CHAT_MODEL
    std::optional<std::string> api_key;  ///< CHAT_API_KEY
};

/// Estado de la lista de modelos.
enum class ModelsState { Idle, Loading, Loaded, Failed };

/// Key enmascarada: "…" y sus últimos 4 caracteres si mide al menos 8 y esos
/// 4 son ASCII imprimibles; si no, solo "…". Nunca expone más de 4.
[[nodiscard]] std::string mask_key(std::string_view key);

/// Estado del formulario "Proveedor de IA", sin FTXUI ni hilos: proveedor,
/// URL base (editable solo en "custom"), key escrita, filtro y lista de
/// modelos, modelo elegido y si hay cambios sin guardar. Los campos
/// definidos por CHAT_* se muestran con su valor efectivo y no se editan.
/// Se usa solo desde el hilo de la interfaz.
class ProviderSettings {
public:
    /// saved: lo que hay en config.json (sin el entorno); credentials: las
    /// keys guardadas; env: las variables CHAT_* definidas.
    ProviderSettings(ConfigFileValues saved, Credentials credentials, SettingsEnv env);

    // --- Proveedor ---
    [[nodiscard]] std::size_t provider() const { return provider_; }
    [[nodiscard]] const ProviderInfo& provider_info() const;
    /// Cambia de proveedor: la URL base se rellena con la de la tabla (en
    /// "custom", la última escrita), la key escrita y el filtro se vacían, la
    /// lista de modelos vuelve a Idle y el modelo es el guardado si se
    /// regresa al proveedor guardado; si no, ninguno.
    void select_provider(std::size_t index);
    /// Nombre para avisos: el de la tabla, o la URL base en "custom".
    [[nodiscard]] std::string provider_label() const;

    // --- URL base ---
    /// Valor del formulario (lo que se guarda en config.json).
    [[nodiscard]] const std::string& base_url() const { return base_url_; }
    /// La que se usa: CHAT_BASE_URL si está definida; si no, la del formulario.
    [[nodiscard]] const std::string& effective_base_url() const;
    [[nodiscard]] bool base_url_locked() const { return env_.base_url.has_value(); }
    [[nodiscard]] bool base_url_editable() const;
    /// false (y no cambia nada) si no es editable.
    bool set_base_url(std::string url);

    // --- Key ---
    /// La key escrita en el formulario (vacía: se conserva la guardada).
    [[nodiscard]] const std::string& key() const { return key_; }
    [[nodiscard]] bool key_locked() const { return env_.api_key.has_value(); }
    /// false (y no cambia nada) si CHAT_API_KEY está definida.
    bool set_key(std::string key);
    /// La que se usa: CHAT_API_KEY, la escrita o la guardada para el
    /// proveedor (y la URL efectiva, en "custom").
    [[nodiscard]] std::string effective_key() const;
    /// Texto junto al campo, siempre enmascarado: "(definido por
    /// CHAT_API_KEY) …abcd", "guardada: …abcd", "opcional en local" o "".
    [[nodiscard]] std::string key_status() const;

    // --- Modelo ---
    [[nodiscard]] const std::string& model() const { return model_; }
    [[nodiscard]] bool model_locked() const { return env_.model.has_value(); }
    /// El que se usa: CHAT_MODEL si está definida; si no, el del formulario.
    [[nodiscard]] const std::string& effective_model() const;
    /// Escrito a mano. false (y no cambia nada) si CHAT_MODEL está definida.
    bool set_model(std::string model);

    // --- Lista de modelos ---
    [[nodiscard]] ModelsState models_state() const { return models_state_; }
    /// Error ya formateado para mostrarse (sin la key).
    [[nodiscard]] const std::string& models_error() const { return models_error_; }
    void models_loading();
    void models_loaded(std::vector<std::string> models);
    void models_failed(std::string message);
    [[nodiscard]] const std::string& filter() const { return filter_; }
    void set_filter(std::string filter);
    /// Los modelos que contienen el filtro, sin distinguir mayúsculas.
    [[nodiscard]] std::vector<std::string> filtered_models() const;
    /// Índice resaltado en filtered_models().
    [[nodiscard]] std::size_t highlighted() const { return highlighted_; }
    /// ↑/↓: mueve el resaltado dentro de filtered_models().
    void move_highlight(int delta);
    /// Enter: el modelo resaltado pasa a ser el elegido. false si no hay o
    /// el modelo está definido por CHAT_MODEL.
    bool pick_highlighted();

    // --- Petición y guardado ---
    /// true si hay URL válida y key (o el host es local): se puede pedir
    /// la lista de modelos.
    [[nodiscard]] bool can_request_models() const;
    /// base con la URL, la key y el modelo efectivos del formulario.
    [[nodiscard]] Config request_config(Config base) const;
    /// Error para mostrar si no se puede guardar (URL, key o modelo), o
    /// nullopt.
    [[nodiscard]] std::optional<std::string> validate() const;
    /// Lo que se escribe en config.json. Con CHAT_MODEL definida se
    /// conserva el modelo guardado.
    [[nodiscard]] ConfigFileValues file_values() const;
    /// Llave y key que se escriben en credentials.json, si se escribió una
    /// (y CHAT_API_KEY no está definida).
    [[nodiscard]] std::optional<std::pair<std::string, std::string>> credential_update() const;
    /// true si el formulario difiere de lo guardado.
    [[nodiscard]] bool dirty() const;
    /// Tras guardar: lo actual pasa a ser lo guardado y la key escrita se
    /// vacía (queda en credentials).
    void mark_saved();

private:
    [[nodiscard]] std::string saved_key() const;

    ConfigFileValues saved_;
    std::size_t saved_provider_ = 0;
    Credentials credentials_;
    SettingsEnv env_;

    std::size_t provider_ = 0;
    std::string base_url_;
    std::string custom_url_; ///< Última URL escrita en "custom".
    std::string key_;
    std::string model_;

    ModelsState models_state_ = ModelsState::Idle;
    std::string models_error_;
    std::vector<std::string> models_;
    std::string filter_;
    std::size_t highlighted_ = 0;

    // Formulario al abrir o tras el último guardado, para dirty().
    std::size_t initial_provider_ = 0;
    std::string initial_url_;
    std::string initial_model_;
};

} // namespace chatbot::cli

#endif // CHATBOT_CLI_PROVIDER_SETTINGS_H
