#include "settings_screen.h"

#include "input_style.h"
#include "list_style.h"
#include "markdown.h"

#include "chatbot/error.h"

#include <ftxui/component/component_options.hpp>
#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <cstddef>
#include <utility>

namespace chatbot::cli {

namespace {

/// Ancho de la columna de etiquetas del formulario.
constexpr int kLabelWidth = 12;
/// Ancho de la columna de categorías ("● Colores y Accesibilidad" y un espacio).
constexpr int kSectionsWidth = 26;
/// Ancho del campo de la key.
constexpr int kKeyWidth = 24;

/// Etiqueta de un campo en su columna; con el foco, con la selección.
ftxui::Element label(const char* text, bool focused, const Palette& palette) {
    return ftxui::hbox({field_label(text, focused, palette)}) |
           ftxui::size(ftxui::WIDTH, ftxui::EQUAL, kLabelWidth);
}

/// Texto de una línea, sin controles de terminal.
std::string clean(std::string_view text) {
    std::string out = md::sanitize(text);
    std::replace(out.begin(), out.end(), '\n', ' ');
    return out;
}

ftxui::InputOption field_option(const Palette& palette, std::function<void()> on_change,
                                std::function<void()> on_enter = [] {}) {
    ftxui::InputOption option;
    option.multiline = false;
    // Sin fondo invertido, igual que la caja de la conversación.
    option.transform = [&palette](ftxui::InputState state) {
        return input_transform(std::move(state.element), state.hovered, state.focused,
                               state.is_placeholder, palette);
    };
    option.on_change = std::move(on_change);
    option.on_enter = std::move(on_enter);
    return option;
}

/// Botón "[ Guardar ]": con el foco, con la selección.
ftxui::ButtonOption button_option(const Palette& palette) {
    ftxui::ButtonOption option;
    option.transform = [&palette](const ftxui::EntryState& state) {
        return button_label(state.label, state.focused, palette);
    };
    return option;
}

/// Filas de una lista de FTXUI (Menu, Radiobox): cursor y elegido como en
/// list_style.h. elegido: state.active en un Menu, state.state en un Radiobox.
std::function<ftxui::Element(const ftxui::EntryState&)> row_transform(const Palette& palette,
                                                                      bool chosen_is_state) {
    return [&palette, chosen_is_state](const ftxui::EntryState& state) {
        return list_row(clean(state.label), state.focused,
                        chosen_is_state ? state.state : state.active, palette);
    };
}

} // namespace

SettingsScreen::SettingsScreen(ModelsLoader& loader, const Palette& palette, OnSave on_save,
                               OnSaveAppearance on_save_appearance, OnClose on_close)
    : loader_(loader), palette_(palette), on_save_(std::move(on_save)),
      on_save_appearance_(std::move(on_save_appearance)), on_close_(std::move(on_close)) {
    category_names_ = {"Proveedor", "Colores y Accesibilidad"};
    for (const Theme& theme : themes()) {
        theme_names_.emplace_back(theme.name);
    }
    background_names_ = {"Del tema", "Transparente"};
    for (const ProviderInfo& provider : providers()) {
        provider_names_.emplace_back(provider.name);
    }

    // Categorías: el cursor y la elegida se mueven juntos (Menu).
    ftxui::MenuOption categories = ftxui::MenuOption::Vertical();
    categories.entries = &category_names_;
    categories.selected = &category_;
    categories.entries_option.transform = row_transform(palette_, false);
    categories_ = ftxui::Menu(categories);

    ftxui::RadioboxOption theme_list;
    theme_list.entries = &theme_names_;
    theme_list.selected = &theme_selected_;
    theme_list.transform = row_transform(palette_, true);
    theme_list_ = ftxui::Radiobox(theme_list);

    ftxui::RadioboxOption background_list;
    background_list.entries = &background_names_;
    background_list.selected = &background_selected_;
    background_list.transform = row_transform(palette_, true);
    background_list_ = ftxui::Radiobox(background_list);

    ftxui::DropdownOption dropdown;
    dropdown.open = &dropdown_open_;
    dropdown.radiobox.entries = &provider_names_;
    dropdown.radiobox.selected = &provider_selected_;
    dropdown.radiobox.on_change = [this] {
        if (!settings_) {
            return;
        }
        loader_.cancel(); // La lista era de otro proveedor.
        settings_->select_provider(static_cast<std::size_t>(provider_selected_));
        sync_fields();
        status_.clear();
    };
    dropdown.radiobox.transform = row_transform(palette_, true);
    dropdown.checkbox.transform = [](const ftxui::EntryState& state) {
        ftxui::Element element = ftxui::hbox({ftxui::text("[ "), ftxui::text(state.label) | ftxui::flex,
                                              ftxui::text(state.state ? " ▴]" : " ▾]")});
        return state.focused ? element | ftxui::bold : element;
    };
    dropdown.transform = [this](bool open, ftxui::Element checkbox, ftxui::Element radiobox) {
        if (!open) {
            return checkbox;
        }
        return ftxui::vbox({std::move(checkbox),
                            palette_.vscroll(std::move(radiobox)) | ftxui::frame |
                                ftxui::size(ftxui::HEIGHT, ftxui::LESS_THAN, 10)});
    };
    dropdown_ = ftxui::Dropdown(dropdown);

    url_input_ = ftxui::Input(&url_text_, "https://mi-servidor/v1",
                              field_option(
                                  palette_,
                                  [this] {
                                      if (settings_ && settings_->set_base_url(url_text_)) {
                                          loader_.cancel(); // La lista era de otra URL.
                                      }
                                  },
                                  [this] { request_models(); }));
    ftxui::InputOption key_option = field_option(
        palette_,
        [this] {
            if (settings_) {
                (void)settings_->set_key(key_text_);
            }
        },
        [this] { request_models(); });
    key_option.password = true; // Nunca se dibuja la key en claro.
    key_input_ = ftxui::Input(&key_text_, "escribe la API key", key_option);
    filter_input_ = ftxui::Input(&filter_text_, "", field_option(
                                                        palette_,
                                                        [this] {
                                                            if (settings_) {
                                                                settings_->set_filter(filter_text_);
                                                            }
                                                        },
                                                        [this] {
                                                            if (settings_ &&
                                                                settings_->pick_highlighted()) {
                                                                model_text_ = settings_->model();
                                                            }
                                                        }));
    refresh_button_ =
        ftxui::Button("Actualizar", [this] { request_models(); }, button_option(palette_));
    model_input_ = ftxui::Input(&model_text_, "escribe el modelo", field_option(palette_, [this] {
                                    if (settings_) {
                                        (void)settings_->set_model(model_text_);
                                    }
                                }));
    save_button_ = ftxui::Button("Guardar", [this] { save(); }, button_option(palette_));
    cancel_button_ =
        ftxui::Button("Cancelar", [this] { (void)request_close(); }, button_option(palette_));

    const ftxui::Component provider_form = ftxui::Container::Vertical({
        dropdown_,
        ftxui::Maybe(url_input_, [this] { return settings_ && settings_->base_url_editable(); }),
        ftxui::Maybe(key_input_, [this] { return settings_ && !settings_->key_locked(); }),
        ftxui::Container::Horizontal({filter_input_, refresh_button_}),
        ftxui::Maybe(model_input_, [this] { return settings_ && !settings_->model_locked(); }),
    });
    const ftxui::Component appearance_form = ftxui::Container::Vertical({
        ftxui::Maybe(theme_list_, [this] { return !no_color_; }),
        // Con un tema sin fondo propio, el fondo no aplica: no toma el foco.
        ftxui::Maybe(background_list_, [this] { return background_enabled(); }),
    });
    const ftxui::Component form = ftxui::Container::Vertical({
        ftxui::Container::Tab({provider_form, appearance_form}, &category_),
        ftxui::Container::Horizontal({save_button_, cancel_button_}),
    });
    root_ = ftxui::CatchEvent(
        ftxui::Renderer(ftxui::Container::Horizontal({categories_, form}),
                        [this] { return render(); }),
        [this](const ftxui::Event& event) { return handle_event(event); });
}

void SettingsScreen::open(ProviderSettings settings, Config base, Appearance appearance,
                          std::string notice) {
    settings_ = std::move(settings);
    base_ = std::move(base);
    no_color_ = no_color_enabled();
    if (no_color_) {
        appearance = resolve_appearance("", "");
    }
    saved_appearance_ = appearance;
    category_ = kProvider;
    theme_selected_ = 0;
    const std::span<const Theme> all = themes();
    for (std::size_t i = 0; i < all.size(); ++i) {
        if (&all[i] == appearance.theme) {
            theme_selected_ = static_cast<int>(i);
        }
    }
    background_selected_ = appearance.background == BackgroundMode::Transparent ? 1 : 0;
    provider_selected_ = static_cast<int>(settings_->provider());
    dropdown_open_ = false;
    confirm_discard_ = false;
    sync_fields();
    status_ = std::move(notice);
    focus();
    if (settings_->can_request_models()) {
        request_models();
    }
}

void SettingsScreen::focus() { categories_->TakeFocus(); }

Appearance SettingsScreen::chosen_appearance() const {
    Appearance appearance;
    appearance.theme = &themes()[static_cast<std::size_t>(theme_selected_)];
    appearance.background =
        background_selected_ == 1 ? BackgroundMode::Transparent : BackgroundMode::FromTheme;
    return appearance;
}

bool SettingsScreen::appearance_dirty() const {
    if (no_color_) {
        return false;
    }
    const Appearance chosen = chosen_appearance();
    return chosen.theme != saved_appearance_.theme ||
           chosen.background != saved_appearance_.background;
}

bool SettingsScreen::background_enabled() const {
    return !no_color_ && has_own_background(themes()[static_cast<std::size_t>(theme_selected_)]);
}

bool SettingsScreen::request_close() {
    if (settings_ && (settings_->dirty() || appearance_dirty())) {
        confirm_discard_ = true;
        return false;
    }
    close();
    return true;
}

void SettingsScreen::close() {
    loader_.cancel();
    settings_.reset();
    key_text_.clear();
    confirm_discard_ = false;
    dropdown_open_ = false;
    status_.clear();
    on_close_();
}

void SettingsScreen::sync_fields() {
    url_text_ = settings_->base_url();
    key_text_ = settings_->key();
    filter_text_ = settings_->filter();
    model_text_ = settings_->model();
}

void SettingsScreen::request_models() {
    if (!settings_) {
        return;
    }
    if (!settings_->can_request_models()) {
        status_ = "Escribe una URL base válida y la API key para pedir los modelos.";
        return;
    }
    status_.clear();
    settings_->models_loading();
    loader_.start(settings_->request_config(base_),
                  [this](const Result<std::vector<std::string>>& result) {
                      if (!settings_) {
                          return;
                      }
                      if (result.is_ok()) {
                          settings_->models_loaded(result.value());
                      } else {
                          settings_->models_failed(
                              std::string{"["} + error_kind_label(result.error().kind) + "] " +
                              result.error().message);
                      }
                  });
}

void SettingsScreen::save() {
    if (!settings_) {
        return;
    }
    const bool appearance_changed = appearance_dirty();
    // Si solo cambió la apariencia, el proveedor no se guarda (ni se
    // reconstruye el cliente); si no cambió nada, se guarda como siempre.
    if (settings_->dirty() || !appearance_changed) {
        if (const std::optional<std::string> error = settings_->validate()) {
            status_ = *error;
            return;
        }
        if (const std::optional<std::string> error = on_save_(*settings_)) {
            status_ = *error;
            return;
        }
        settings_->mark_saved();
    }
    if (appearance_changed) {
        const Appearance chosen = chosen_appearance();
        if (const std::optional<std::string> error = on_save_appearance_(chosen)) {
            status_ = *error;
            return;
        }
        saved_appearance_ = chosen;
    }
    close();
}

void SettingsScreen::move_focus(int step) {
    // Las categorías, los campos de la categoría elegida y los botones.
    std::vector<ftxui::Component> order{categories_};
    if (category_ == kAppearance) {
        if (!no_color_) {
            order.push_back(theme_list_);
        }
        if (background_enabled()) {
            order.push_back(background_list_);
        }
    } else {
        order.push_back(dropdown_);
        if (settings_->base_url_editable()) {
            order.push_back(url_input_);
        }
        if (!settings_->key_locked()) {
            order.push_back(key_input_);
        }
        order.push_back(filter_input_);
        order.push_back(refresh_button_);
        if (!settings_->model_locked()) {
            order.push_back(model_input_);
        }
    }
    order.push_back(save_button_);
    order.push_back(cancel_button_);
    const auto count = static_cast<long long>(order.size());
    long long current = 0;
    for (long long i = 0; i < count; ++i) {
        if (order[static_cast<std::size_t>(i)]->Focused()) {
            current = i;
            break;
        }
    }
    current = ((current + step) % count + count) % count;
    order[static_cast<std::size_t>(current)]->TakeFocus();
}

bool SettingsScreen::handle_event(const ftxui::Event& event) {
    if (!settings_) {
        return false;
    }
    if (confirm_discard_) {
        if (event.is_mouse()) {
            return true; // Primero se contesta la pregunta.
        }
        if (event.is_character() && (event.character() == "s" || event.character() == "S")) {
            close();
        } else {
            confirm_discard_ = false;
        }
        return true;
    }
    if (dropdown_open_) {
        if (event != ftxui::Event::Tab && event != ftxui::Event::TabReverse) {
            return false; // ↑/↓, Enter y Esc son de la lista abierta.
        }
        dropdown_open_ = false;
    }
    if (event == ftxui::Event::Escape) {
        (void)request_close();
        return true;
    }
    if (event == ftxui::Event::Tab || event == ftxui::Event::TabReverse) {
        move_focus(event == ftxui::Event::Tab ? 1 : -1);
        return true;
    }
    if (category_ == kProvider && event.is_mouse() &&
        settings_->models_state() == ModelsState::Loaded && !model_row_boxes_.empty()) {
        auto mouse_event = event;
        const auto& mouse = mouse_event.mouse();
        if (models_box_.Contain(mouse.x, mouse.y)) {
            if (mouse.button == ftxui::Mouse::WheelUp || mouse.button == ftxui::Mouse::WheelDown) {
                filter_input_->TakeFocus();
                settings_->move_highlight(mouse.button == ftxui::Mouse::WheelUp ? -1 : 1);
                return true;
            }
            if (mouse.button == ftxui::Mouse::Left && mouse.motion == ftxui::Mouse::Pressed) {
                for (std::size_t i = 0; i < model_row_boxes_.size(); ++i) {
                    if (model_row_boxes_[i].Contain(mouse.x, mouse.y)) {
                        filter_input_->TakeFocus();
                        settings_->move_highlight(static_cast<int>(i) -
                                                   static_cast<int>(settings_->highlighted()));
                        if (settings_->pick_highlighted()) {
                            model_text_ = settings_->model();
                        }
                        return true;
                    }
                }
            }
        }
    }
    if (category_ == kProvider && filter_input_->Focused()) {
        // Con el filtro, ↑/↓ (y PgUp/PgDn) mueven el resaltado de la lista.
        const std::pair<const ftxui::Event*, int> moves[] = {
            {&ftxui::Event::ArrowUp, -1},
            {&ftxui::Event::ArrowDown, 1},
            {&ftxui::Event::PageUp, -10},
            {&ftxui::Event::PageDown, 10},
        };
        for (const auto& [move, delta] : moves) {
            if (event == *move) {
                settings_->move_highlight(delta);
                return true;
            }
        }
    }
    return false;
}

ftxui::Element SettingsScreen::render_models() const {
    model_row_boxes_.clear();
    const ProviderSettings& s = *settings_;
    const ftxui::Decorator notice = palette_.ink(&Theme::notice);
    switch (s.models_state()) {
    case ModelsState::Idle:
        return ftxui::text(s.can_request_models()
                               ? "Pulsa Actualizar para pedir la lista."
                               : "Escribe la URL base y la API key, y pulsa Actualizar.") |
               notice;
    case ModelsState::Loading:
        return ftxui::text("Cargando modelos…");
    case ModelsState::Failed:
        return ftxui::vbox({
            ftxui::paragraph(clean(s.models_error())) | ftxui::bold,
            ftxui::text(s.model_locked() ? "El modelo lo define CHAT_MODEL."
                                         : "Puedes escribir el modelo a mano en «Modelo».") |
                notice,
        });
    case ModelsState::Loaded:
        break;
    }
    const std::vector<std::string> models = s.filtered_models();
    if (models.empty()) {
        return ftxui::text(s.filter().empty() ? "El servidor no devolvió modelos."
                                              : "Ningún modelo coincide con el filtro.") |
               notice;
    }
    std::vector<std::string> labels;
    std::optional<std::size_t> chosen;
    labels.reserve(models.size());
    for (std::size_t i = 0; i < models.size(); ++i) {
        labels.push_back(clean(models[i]));
        if (models[i] == s.effective_model()) {
            chosen = i;
        }
    }
    // La lista tiene el foco mientras lo tiene el filtro: ↑/↓ mueven el cursor.
    return choice_list(labels, s.highlighted(), chosen, filter_input_->Focused(), palette_,
                        &model_row_boxes_);
}

ftxui::Element SettingsScreen::render_provider() const {
    const ProviderSettings& s = *settings_;
    const ftxui::Decorator notice = palette_.ink(&Theme::notice);

    ftxui::Element url;
    if (s.base_url_editable()) {
        url = url_input_->Render() | ftxui::flex;
    } else {
        url = ftxui::hbox({
            ftxui::text(clean(s.effective_base_url())) | ftxui::flex_shrink,
            s.base_url_locked() ? ftxui::text("  (definido por CHAT_BASE_URL)") | notice
                                : ftxui::emptyElement(),
        });
    }

    // La key nunca se dibuja: el campo muestra "•" y el estado, enmascarado.
    ftxui::Element key;
    if (s.key_locked()) {
        key = ftxui::text(s.key_status()) | notice;
    } else {
        key = ftxui::hbox({
            key_input_->Render() | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, kKeyWidth),
            ftxui::text("  " + s.key_status()) | notice,
        });
    }

    ftxui::Element model;
    if (s.model_locked()) {
        model = ftxui::hbox({ftxui::text(clean(s.effective_model())),
                             ftxui::text("  (definido por CHAT_MODEL)") | notice});
    } else {
        model = model_input_->Render() | ftxui::flex;
    }

    return ftxui::vbox({
        ftxui::text("Proveedor de IA") | ftxui::bold,
        ftxui::text(""),
        ftxui::hbox({label("Proveedor", dropdown_->Focused(), palette_),
                     dropdown_->Render() | ftxui::flex}),
        ftxui::hbox({label("URL base", url_input_->Focused(), palette_), std::move(url)}),
        ftxui::hbox({label("API key", key_input_->Focused(), palette_), std::move(key)}),
        ftxui::text(""),
        ftxui::hbox({label("Modelos", false, palette_),
                     field_label("Filtrar:", filter_input_->Focused(), palette_),
                     ftxui::text(" "), filter_input_->Render() | ftxui::flex, ftxui::text(" "),
                     refresh_button_->Render()}),
        render_models() | ftxui::flex | ftxui::reflect(models_box_),
        ftxui::hbox({label("Modelo", model_input_->Focused(), palette_), std::move(model)}),
    });
}

ftxui::Element SettingsScreen::render_appearance() const {
    const ftxui::Decorator notice = palette_.ink(&Theme::notice);
    if (no_color_) {
        return ftxui::vbox({
            ftxui::text("Colores y Accesibilidad") | ftxui::bold,
            ftxui::text(""),
            ftxui::hbox({label("Tema", false, palette_), ftxui::text("De la terminal") | notice}),
            ftxui::hbox({label("Fondo", false, palette_), ftxui::text("Transparente") | notice}),
            ftxui::text("Desactivado por NO_COLOR") | notice,
            ftxui::filler(),
        });
    }
    ftxui::Element background;
    if (background_enabled()) {
        background = background_list_->Render();
    } else {
        // No aplica: las opciones, sin cursor ni elegido, en texto secundario.
        ftxui::Elements rows;
        for (const std::string& name : background_names_) {
            rows.push_back(ftxui::text(std::string{kNotChosenMark} + name) | notice);
        }
        rows.push_back(ftxui::text("No aplica: este tema usa el fondo de la terminal.") | notice);
        background = ftxui::vbox(std::move(rows));
    }
    return ftxui::vbox({
        ftxui::text("Colores y Accesibilidad") | ftxui::bold,
        ftxui::text(""),
        ftxui::hbox({label("Tema", theme_list_->Focused(), palette_),
                     theme_list_->Render() | ftxui::flex}),
        ftxui::text(""),
        ftxui::hbox({label("Fondo", background_list_->Focused(), palette_),
                     std::move(background) | ftxui::flex}),
        ftxui::text(""),
        ftxui::text("Se aplica al guardar.") | notice,
        ftxui::filler(),
    });
}

ftxui::Element SettingsScreen::render() const {
    if (!settings_) {
        return ftxui::emptyElement();
    }
    ftxui::Element status;
    if (confirm_discard_) {
        status = ftxui::text("¿Descartar los cambios? (s/n)") | ftxui::bold;
    } else if (!status_.empty()) {
        status = ftxui::paragraph(clean(status_)) | ftxui::bold;
    } else if (category_ == kAppearance) {
        status = ftxui::text("Tab: campo · ↑/↓: mover · Enter: elegir · Esc: cerrar") |
                 palette_.ink(&Theme::notice);
    } else {
        status = ftxui::text("Tab: campo · ↑/↓ y Enter: modelo · Esc: cerrar") |
                 palette_.ink(&Theme::notice);
    }

    ftxui::Element form = ftxui::vbox({
        (category_ == kAppearance ? render_appearance() : render_provider()) | ftxui::flex,
        ftxui::text(""),
        ftxui::hbox({ftxui::filler(), save_button_->Render(), ftxui::text("  "),
                     cancel_button_->Render()}),
        std::move(status),
    });
    ftxui::Element sections =
        categories_->Render() | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, kSectionsWidth);
    // El borde y los separadores en el color border; lo de adentro, no.
    ftxui::Element body = ftxui::hbox({std::move(sections),
                                       ftxui::separator() | palette_.ink(&Theme::border),
                                       ftxui::text(" "), std::move(form) | ftxui::flex}) |
                          palette_.inside_border();
    return ftxui::window(ftxui::text(" Configuración ") | ftxui::bold | palette_.inside_border(),
                         std::move(body), ftxui::LIGHT) |
           palette_.ink(&Theme::border) | ftxui::flex;
}

} // namespace chatbot::cli
