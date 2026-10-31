#ifndef CHATBOT_CLI_ADDONS_PANEL_H
#define CHATBOT_CLI_ADDONS_PANEL_H

#include "addon_form.h"
#include "telegram_addon.h"
#include "theme.h"

#include "chatbot/addon_settings/registry.h"
#include "chatbot/addon_settings/telegram_section.h"
#include "chatbot/platform.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace chatbot::cli {

/// F2 → Addons → Telegram (FTXUI): el estado del addon y de su servicio
/// arriba, fijo, y debajo los campos (con barra si no caben) y los
/// invitados. Lo guardado, guardar y el servicio están en TelegramAddon
/// (sin FTXUI, lo comparte chatbot-rpc); aquí, el dibujo, el foco, los
/// textos y las preguntas, que salen en la línea de estado de
/// SettingsScreen. Se usa solo desde el hilo de la interfaz.
class AddonsPanel {
public:
    /// Un aviso para la línea de estado.
    using Notice = std::function<void(std::string)>;
    /// Una pregunta (s/n) para la línea de estado.
    struct Question {
        std::string text;
        std::function<void()> yes;
        std::function<void()> no;
        std::string cancelled; ///< El aviso de Esc, que no hace nada.
    };

    /// palette debe vivir más que el panel. retry_save: vuelve a guardar
    /// (después de confirmar "Se descartarán: …").
    AddonsPanel(const Palette& palette, Notice notice, std::function<void()> retry_save);
    ~AddonsPanel();
    AddonsPanel(const AddonsPanel&) = delete;
    AddonsPanel& operator=(const AddonsPanel&) = delete;
    AddonsPanel(AddonsPanel&&) = delete;
    AddonsPanel& operator=(AddonsPanel&&) = delete;

    /// Una vez, al arrancar. Sin llamarla (o en Windows) el panel solo
    /// muestra que los addons no están disponibles.
    void set_environment(AddonsEnvironment environment);

    /// Lee la sección, los secretos y los invitados. app_model: el de la app,
    /// para "Mismo que la app (…)".
    void open(const std::string& app_model);
    /// Se eligió la categoría: pide el estado (una vez por apertura).
    void show();
    /// Olvida la pregunta pendiente.
    void close() { question_.reset(); }

    [[nodiscard]] bool dirty() const;
    /// Guardar haría algo: hay cambios o el servicio no está como dice
    /// Activado.
    [[nodiscard]] bool pending() const;
    /// El error que impide guardar ("<etiqueta>: …"), o nullopt.
    [[nodiscard]] std::optional<std::string> check() const;
    /// Si hay llaves que se descartarían, pregunta y devuelve true: guardar
    /// espera la respuesta (retry_save).
    bool ask_before_saving();
    /// Escribe credentials.json y la sección (si cambiaron), y después lleva
    /// el servicio a lo que dice Activado en su hilo (con preguntas si hacen
    /// falta). El error, o nullopt.
    [[nodiscard]] std::optional<std::string> commit();

    [[nodiscard]] const std::optional<Question>& question() const { return question_; }
    void answer(bool yes);
    /// Esc: olvida la pregunta sin hacer nada y lo dice.
    void cancel_question();

    [[nodiscard]] ftxui::Component component() const { return root_; }
    /// Lo que recorre Tab, en orden.
    [[nodiscard]] std::vector<ftxui::Component> focus_order() const;
    /// Supr sobre un invitado. true si lo usó.
    bool handle_event(const ftxui::Event& event);
    /// Un campo de texto tiene el foco.
    [[nodiscard]] bool text_field_focused() const;
    [[nodiscard]] ftxui::Element render() const;

private:
    [[nodiscard]] bool available() const;
    [[nodiscard]] bool read_only() const;
    /// TelegramAddon::out_of_sync con la casilla Activado.
    [[nodiscard]] std::string out_of_sync() const;
    void refresh_placeholders();
    void refresh_status();
    void apply_service(bool restart, bool replace_foreign);
    void ask_remove_invited();
    void set_invited(std::vector<std::int64_t> users);
    [[nodiscard]] ftxui::Element render_block() const;
    [[nodiscard]] ftxui::Element render_fields() const;
    [[nodiscard]] ftxui::Element render_field(AddonField field) const;
    [[nodiscard]] ftxui::Element render_invited() const;

    const Palette& palette_;
    Notice notice_;
    std::function<void()> retry_save_;
    const addon_settings::AddonInfo& addon_;
    /// Sin set_environment, nullptr.
    std::unique_ptr<TelegramAddon> telegram_;

    AddonForm form_; ///< Lo que editan los componentes.
    bool status_requested_ = false;
    bool discard_confirmed_ = false;
    std::optional<Question> question_;
    std::vector<std::int64_t> invited_;
    std::vector<std::string> invited_names_;
    int invited_selected_ = 0;
    std::string app_model_; ///< El de la app (open), para "Modelo".
    std::string token_placeholder_;
    std::string model_key_placeholder_;
    std::string search_key_placeholder_;

    std::array<ftxui::Component, kAddonFieldCount> inputs_;
    ftxui::Component enabled_box_;
    ftxui::Component details_box_;
    ftxui::Component remove_model_key_;
    ftxui::Component remove_search_key_;
    ftxui::Component generate_code_;
    ftxui::Component restart_button_;
    ftxui::Component refresh_button_;
    ftxui::Component invited_menu_;
    ftxui::Component root_;
};

} // namespace chatbot::cli

#endif // CHATBOT_CLI_ADDONS_PANEL_H
