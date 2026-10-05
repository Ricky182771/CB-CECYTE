#ifndef CHATBOT_CLI_SETTINGS_SCREEN_H
#define CHATBOT_CLI_SETTINGS_SCREEN_H

#include "models_loader.h"
#include "provider_settings.h"
#include "theme.h"

#include "chatbot/config.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace chatbot::cli {

/// Pantalla de configuración (FTXUI): categorías a la izquierda
/// ("Proveedor" y "Colores y Accesibilidad") y el formulario de la elegida a
/// la derecha. La lógica del proveedor está en ProviderSettings; la lista de
/// modelos se pide con ModelsLoader, en su hilo. Se dibuja con la paleta
/// vigente (la de main.cpp, que cambia al guardar). Se usa solo desde el
/// hilo de la interfaz.
class SettingsScreen {
public:
    /// Guarda lo del formulario (config.json, credentials.json) y reconstruye
    /// el cliente. Devuelve el error para mostrar, o nullopt si se guardó.
    using OnSave = std::function<std::optional<std::string>(const ProviderSettings&)>;
    /// Guarda el tema y el fondo en config.json y los aplica. Devuelve el
    /// error para mostrar, o nullopt si se guardó.
    using OnSaveAppearance = std::function<std::optional<std::string>(const Appearance&)>;
    /// La pantalla se cerró (guardada o descartada).
    using OnClose = std::function<void()>;

    /// palette debe vivir más que la pantalla.
    SettingsScreen(ModelsLoader& loader, const Palette& palette, OnSave on_save,
                   OnSaveAppearance on_save_appearance, OnClose on_close);

    /// Abre con un formulario nuevo. base: la configuración actual (timeout,
    /// volcado) para pedir los modelos. appearance: el tema y el fondo
    /// vigentes. notice: aviso inicial, si hay. Pide la lista de modelos si
    /// ya hay URL y key.
    void open(ProviderSettings settings, Config base, Appearance appearance,
              std::string notice = {});
    /// Esc, Cancelar o F2: cierra si no hay cambios; si los hay, pregunta
    /// "¿Descartar los cambios? (s/n)". Devuelve true si se cerró.
    bool request_close();
    /// Cierra sin preguntar y cancela la petición de modelos en curso.
    void close();

    [[nodiscard]] bool is_open() const { return settings_.has_value(); }
    /// El componente (para Container::Tab).
    [[nodiscard]] ftxui::Component component() const { return root_; }
    /// Da el foco al primer campo.
    void focus();

private:
    /// Categorías del menú de la izquierda.
    enum Category : int { kProvider = 0, kAppearance = 1 };

    void request_models();
    void save();
    /// Copia al formulario lo que cambió en settings_ (tras cambiar de proveedor).
    void sync_fields();
    /// Tab y Shift+Tab: siguiente campo enfocable.
    void move_focus(int step);
    bool handle_event(const ftxui::Event& event);
    [[nodiscard]] ftxui::Element render() const;
    [[nodiscard]] ftxui::Element render_models() const;
    [[nodiscard]] ftxui::Element render_provider() const;
    [[nodiscard]] ftxui::Element render_appearance() const;
    /// Tema y fondo elegidos en el formulario.
    [[nodiscard]] Appearance chosen_appearance() const;
    [[nodiscard]] bool appearance_dirty() const;
    /// El fondo se puede elegir (el tema elegido tiene fondo propio).
    [[nodiscard]] bool background_enabled() const;

    ModelsLoader& loader_;
    const Palette& palette_;
    OnSave on_save_;
    OnSaveAppearance on_save_appearance_;
    OnClose on_close_;
    std::optional<ProviderSettings> settings_;
    Config base_;
    Appearance saved_appearance_; ///< El vigente al abrir (o al guardar).
    std::string status_;          ///< Aviso o error bajo el formulario.
    bool confirm_discard_ = false; ///< Se preguntó "¿Descartar los cambios?".
    bool no_color_ = false;

    // Lo que editan los componentes.
    std::vector<std::string> category_names_;
    int category_ = kProvider;
    std::vector<std::string> theme_names_;
    int theme_selected_ = 0;
    std::vector<std::string> background_names_;
    int background_selected_ = 0;
    std::vector<std::string> provider_names_;
    int provider_selected_ = 0;
    bool dropdown_open_ = false;
    std::string url_text_;
    std::string key_text_;
    std::string filter_text_;
    std::string model_text_;
    mutable std::vector<ftxui::Box> model_row_boxes_;
    mutable ftxui::Box models_box_{0, -1, 0, -1};

    ftxui::Component categories_;
    ftxui::Component theme_list_;
    ftxui::Component background_list_;
    ftxui::Component dropdown_;
    ftxui::Component url_input_;
    ftxui::Component key_input_;
    ftxui::Component filter_input_;
    ftxui::Component refresh_button_;
    ftxui::Component model_input_;
    ftxui::Component save_button_;
    ftxui::Component cancel_button_;
    ftxui::Component root_;
};

} // namespace chatbot::cli

#endif // CHATBOT_CLI_SETTINGS_SCREEN_H
