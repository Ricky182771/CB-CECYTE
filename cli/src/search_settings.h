#ifndef CHATBOT_CLI_SEARCH_SETTINGS_H
#define CHATBOT_CLI_SEARCH_SETTINGS_H

#include <optional>
#include <string>
#include <utility>

namespace chatbot::cli {

/// Estado del formulario "Búsqueda web", sin FTXUI: la key de Tavily
/// escrita, la guardada en credentials.json ("search:tavily") y la de
/// CHAT_SEARCH_API_KEY, que bloquea el campo. La key nunca se muestra en
/// claro (mask_key). Se usa solo desde el hilo de la interfaz.
class SearchSettings {
public:
    SearchSettings() = default;
    /// saved: la de credentials.json (vacía si no hay); env: la de
    /// CHAT_SEARCH_API_KEY si está definida y no vacía.
    SearchSettings(std::string saved, std::optional<std::string> env);

    /// La key escrita en el formulario (vacía: se conserva la guardada).
    [[nodiscard]] const std::string& key() const { return key_; }
    [[nodiscard]] bool key_locked() const { return env_.has_value(); }
    /// false (y no cambia nada) si CHAT_SEARCH_API_KEY está definida.
    bool set_key(std::string key);
    /// Texto junto al campo, siempre enmascarado: "(definido por
    /// CHAT_SEARCH_API_KEY) …abcd", "guardada: …abcd", "sin configurar" o ""
    /// (mientras se escribe una).
    [[nodiscard]] std::string key_status() const;

    /// Error para mostrar si la key escrita no parece completa (misma regla
    /// que la del modelo, plausible_key), o nullopt. Nunca incluye la key.
    [[nodiscard]] std::optional<std::string> validate() const;
    /// Llave y key que se escriben en credentials.json, si se escribió una
    /// válida y CHAT_SEARCH_API_KEY no está definida.
    [[nodiscard]] std::optional<std::pair<std::string, std::string>> credential_update() const;
    /// true si se escribió una key.
    [[nodiscard]] bool dirty() const { return !key_.empty(); }
    /// Tras guardar: la escrita pasa a ser la guardada y el campo se vacía.
    void mark_saved();

private:
    std::string saved_;
    std::optional<std::string> env_;
    std::string key_;
};

} // namespace chatbot::cli

#endif // CHATBOT_CLI_SEARCH_SETTINGS_H
