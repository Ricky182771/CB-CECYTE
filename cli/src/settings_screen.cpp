#include "settings_screen.h"

#include "input_style.h"
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
/// Ancho de la columna de secciones.
constexpr int kSectionsWidth = 16;
/// Ancho del campo de la key.
constexpr int kKeyWidth = 24;

ftxui::Element label(const char* text) {
    return ftxui::text(text) | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, kLabelWidth);
}

/// Texto de una línea, sin controles de terminal.
std::string clean(std::string_view text) {
    std::string out = md::sanitize(text);
    std::replace(out.begin(), out.end(), '\n', ' ');
    return out;
}

ftxui::InputOption field_option(std::function<void()> on_change,
                                std::function<void()> on_enter = [] {}) {
    ftxui::InputOption option;
    option.multiline = false;
    // Sin fondo invertido, igual que la caja de la conversación.
    option.transform = [](ftxui::InputState state) {
        return input_transform(std::move(state.element), state.hovered, state.focused,
                               state.is_placeholder);
    };
    option.on_change = std::move(on_change);
    option.on_enter = std::move(on_enter);
    return option;
}

/// Botón "[ Guardar ]": con el foco, en negritas y subrayado (sin invertir).
ftxui::ButtonOption button_option() {
    ftxui::ButtonOption option;
    option.transform = [](const ftxui::EntryState& state) {
        ftxui::Element element = ftxui::text("[ " + state.label + " ]");
        return state.focused ? element | ftxui::bold | ftxui::underlined : element;
    };
    return option;
}

} // namespace

SettingsScreen::SettingsScreen(ModelsLoader& loader, OnSave on_save, OnClose on_close)
    : loader_(loader), on_save_(std::move(on_save)), on_close_(std::move(on_close)) {
    for (const ProviderInfo& provider : providers()) {
        provider_names_.emplace_back(provider.name);
    }

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
    dropdown.radiobox.transform = [](const ftxui::EntryState& state) {
        ftxui::Element element =
            ftxui::text((state.focused ? "▸ " : "  ") + state.label);
        return state.state ? element | ftxui::bold : element;
    };
    dropdown.checkbox.transform = [](const ftxui::EntryState& state) {
        ftxui::Element element = ftxui::hbox({ftxui::text("[ "), ftxui::text(state.label) | ftxui::flex,
                                              ftxui::text(state.state ? " ▴]" : " ▾]")});
        return state.focused ? element | ftxui::bold : element;
    };
    dropdown.transform = [](bool open, ftxui::Element checkbox, ftxui::Element radiobox) {
        if (!open) {
            return checkbox;
        }
        return ftxui::vbox({std::move(checkbox),
                            std::move(radiobox) | ftxui::vscroll_indicator | ftxui::frame |
                                ftxui::size(ftxui::HEIGHT, ftxui::LESS_THAN, 10)});
    };
    dropdown_ = ftxui::Dropdown(dropdown);

    url_input_ = ftxui::Input(&url_text_, "https://mi-servidor/v1",
                              field_option(
                                  [this] {
                                      if (settings_ && settings_->set_base_url(url_text_)) {
                                          loader_.cancel(); // La lista era de otra URL.
                                      }
                                  },
                                  [this] { request_models(); }));
    ftxui::InputOption key_option = field_option(
        [this] {
            if (settings_) {
                (void)settings_->set_key(key_text_);
            }
        },
        [this] { request_models(); });
    key_option.password = true; // Nunca se dibuja la key en claro.
    key_input_ = ftxui::Input(&key_text_, "escribe la API key", key_option);
    filter_input_ = ftxui::Input(&filter_text_, "", field_option(
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
    refresh_button_ = ftxui::Button("Actualizar", [this] { request_models(); }, button_option());
    model_input_ = ftxui::Input(&model_text_, "escribe el modelo", field_option([this] {
                                    if (settings_) {
                                        (void)settings_->set_model(model_text_);
                                    }
                                }));
    save_button_ = ftxui::Button("Guardar", [this] { save(); }, button_option());
    cancel_button_ = ftxui::Button("Cancelar", [this] { (void)request_close(); }, button_option());

    const ftxui::Component form = ftxui::Container::Vertical({
        dropdown_,
        ftxui::Maybe(url_input_, [this] { return settings_ && settings_->base_url_editable(); }),
        ftxui::Maybe(key_input_, [this] { return settings_ && !settings_->key_locked(); }),
        ftxui::Container::Horizontal({filter_input_, refresh_button_}),
        ftxui::Maybe(model_input_, [this] { return settings_ && !settings_->model_locked(); }),
        ftxui::Container::Horizontal({save_button_, cancel_button_}),
    });
    root_ = ftxui::CatchEvent(ftxui::Renderer(form, [this] { return render(); }),
                              [this](const ftxui::Event& event) { return handle_event(event); });
}

void SettingsScreen::open(ProviderSettings settings, Config base, std::string notice) {
    settings_ = std::move(settings);
    base_ = std::move(base);
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

void SettingsScreen::focus() { dropdown_->TakeFocus(); }

bool SettingsScreen::request_close() {
    if (settings_ && settings_->dirty()) {
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
    if (const std::optional<std::string> error = settings_->validate()) {
        status_ = *error;
        return;
    }
    if (const std::optional<std::string> error = on_save_(*settings_)) {
        status_ = *error;
        return;
    }
    settings_->mark_saved();
    close();
}

void SettingsScreen::move_focus(int step) {
    const std::vector<ftxui::Component> order = {
        dropdown_,    url_input_,     key_input_,   filter_input_,
        refresh_button_, model_input_, save_button_, cancel_button_,
    };
    const auto visible = [this](std::size_t i) {
        switch (i) {
        case 1:
            return settings_->base_url_editable();
        case 2:
            return !settings_->key_locked();
        case 5:
            return !settings_->model_locked();
        default:
            return true;
        }
    };
    const auto count = static_cast<long long>(order.size());
    long long current = 0;
    for (long long i = 0; i < count; ++i) {
        if (order[static_cast<std::size_t>(i)]->Focused()) {
            current = i;
            break;
        }
    }
    for (long long tries = 0; tries < count; ++tries) {
        current = ((current + step) % count + count) % count;
        if (visible(static_cast<std::size_t>(current))) {
            order[static_cast<std::size_t>(current)]->TakeFocus();
            return;
        }
    }
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
    if (filter_input_->Focused()) {
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
    const ProviderSettings& s = *settings_;
    switch (s.models_state()) {
    case ModelsState::Idle:
        return ftxui::text(s.can_request_models()
                               ? "Pulsa Actualizar para pedir la lista."
                               : "Escribe la URL base y la API key, y pulsa Actualizar.") |
               ftxui::dim;
    case ModelsState::Loading:
        return ftxui::text("Cargando modelos…");
    case ModelsState::Failed:
        return ftxui::vbox({
            ftxui::paragraph(clean(s.models_error())) | ftxui::bold,
            ftxui::text(s.model_locked() ? "El modelo lo define CHAT_MODEL."
                                         : "Puedes escribir el modelo a mano en «Modelo».") |
                ftxui::dim,
        });
    case ModelsState::Loaded:
        break;
    }
    const std::vector<std::string> models = s.filtered_models();
    if (models.empty()) {
        return ftxui::text(s.filter().empty() ? "El servidor no devolvió modelos."
                                              : "Ningún modelo coincide con el filtro.") |
               ftxui::dim;
    }
    const bool filtering = filter_input_->Focused();
    ftxui::Elements lines;
    for (std::size_t i = 0; i < models.size(); ++i) {
        const bool chosen = models[i] == s.effective_model();
        ftxui::Element line = ftxui::text((chosen ? "● " : "  ") + clean(models[i]));
        if (i == s.highlighted()) {
            line = line | ftxui::bold | ftxui::focus;
            if (filtering) {
                line = line | ftxui::underlined;
            }
        }
        lines.push_back(std::move(line));
    }
    return ftxui::vbox(std::move(lines)) | ftxui::vscroll_indicator | ftxui::frame;
}

ftxui::Element SettingsScreen::render() const {
    if (!settings_) {
        return ftxui::emptyElement();
    }
    const ProviderSettings& s = *settings_;

    ftxui::Element url;
    if (s.base_url_editable()) {
        url = url_input_->Render() | ftxui::flex;
    } else {
        url = ftxui::hbox({
            ftxui::text(clean(s.effective_base_url())) | ftxui::flex_shrink,
            s.base_url_locked() ? ftxui::text("  (definido por CHAT_BASE_URL)") | ftxui::dim
                                : ftxui::emptyElement(),
        });
    }

    // La key nunca se dibuja: el campo muestra "•" y el estado, enmascarado.
    ftxui::Element key;
    if (s.key_locked()) {
        key = ftxui::text(s.key_status()) | ftxui::dim;
    } else {
        key = ftxui::hbox({
            key_input_->Render() | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, kKeyWidth),
            ftxui::text("  " + s.key_status()) | ftxui::dim,
        });
    }

    ftxui::Element model;
    if (s.model_locked()) {
        model = ftxui::hbox({ftxui::text(clean(s.effective_model())),
                             ftxui::text("  (definido por CHAT_MODEL)") | ftxui::dim});
    } else {
        model = model_input_->Render() | ftxui::flex;
    }

    ftxui::Element status;
    if (confirm_discard_) {
        status = ftxui::text("¿Descartar los cambios? (s/n)") | ftxui::bold;
    } else if (!status_.empty()) {
        status = ftxui::paragraph(clean(status_)) | ftxui::bold;
    } else {
        status = ftxui::text("Tab: campo · ↑/↓ y Enter: modelo · Esc: cerrar") |
                 ftxui::dim;
    }

    ftxui::Element form = ftxui::vbox({
        ftxui::text("Proveedor de IA") | ftxui::bold,
        ftxui::text(""),
        ftxui::hbox({label("Proveedor"), dropdown_->Render() | ftxui::flex}),
        ftxui::hbox({label("URL base"), std::move(url)}),
        ftxui::hbox({label("API key"), std::move(key)}),
        ftxui::text(""),
        ftxui::hbox({label("Modelos"), ftxui::text("Filtrar: "), filter_input_->Render() | ftxui::flex,
                     ftxui::text(" "), refresh_button_->Render()}),
        render_models() | ftxui::flex,
        ftxui::hbox({label("Modelo"), std::move(model)}),
        ftxui::text(""),
        ftxui::hbox({ftxui::filler(), save_button_->Render(), ftxui::text("  "),
                     cancel_button_->Render()}),
        std::move(status),
    });
    ftxui::Element sections =
        ftxui::vbox({ftxui::text("Proveedor") | ftxui::bold}) |
        ftxui::size(ftxui::WIDTH, ftxui::EQUAL, kSectionsWidth);
    return ftxui::window(ftxui::text(" Configuración ") | ftxui::bold,
                         ftxui::hbox({std::move(sections), ftxui::separator(),
                                      ftxui::text(" "), std::move(form) | ftxui::flex}),
                         ftxui::LIGHT) |
           ftxui::flex;
}

} // namespace chatbot::cli
