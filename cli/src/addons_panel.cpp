#include "addons_panel.h"

#include "input_style.h"
#include "list_style.h"

#include "chatbot/addon_settings/invited.h"
#include "chatbot/addon_settings/paths.h"
#include "chatbot/addon_settings/secrets.h"
#include "chatbot/addon_settings/time_zone.h"
#include "chatbot/addon_settings/token.h"
#include "chatbot/credentials.h"
#include "chatbot/markdown/markdown.h"

#include <ftxui/component/component_options.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/dom/requirement.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/string.hpp>

#include <algorithm>
#include <utility>

namespace chatbot::cli {
namespace {

namespace settings = addon_settings;

/// Ancho de la columna de etiquetas (las etiquetas miden ≤ 20).
constexpr int kLabelWidth = 21;
/// Filas máximas del bloque fijo de arriba (estado, servicio, botones,
/// linger, registro).
constexpr int kBlockRows = 6;
/// Filas mínimas de los campos; con menos, se desplaza todo el panel.
constexpr int kMinFieldRows = 5;

std::string clean(std::string_view text) {
    std::string out = md::sanitize(text);
    std::replace(out.begin(), out.end(), '\n', ' ');
    return out;
}

ftxui::Element label(std::string_view text, bool focused, const Palette& palette) {
    return field_label(text, focused, palette) |
           ftxui::size(ftxui::WIDTH, ftxui::EQUAL, kLabelWidth);
}

ftxui::ButtonOption button_option(const Palette& palette) {
    ftxui::ButtonOption option;
    option.transform = [&palette](const ftxui::EntryState& state) {
        return button_label(state.label, state.focused, palette);
    };
    return option;
}

ftxui::CheckboxOption check_option(const Palette& palette) {
    ftxui::CheckboxOption option;
    option.transform = [&palette](const ftxui::EntryState& state) {
        return field_label((state.state ? "[x] " : "[ ] ") + state.label, state.focused, palette);
    };
    return option;
}

/// Una nota con sangría de 2 en todas sus filas (paragraph se come los
/// espacios del principio).
ftxui::Element indented_note(std::string text) {
    return ftxui::hbox({ftxui::text("  "), ftxui::paragraph(std::move(text)) | ftxui::flex});
}

/// Una línea que, si no cabe, se recorta por el principio con "…" (una
/// ruta: lo que importa es el final).
class TailText : public ftxui::Node {
public:
    explicit TailText(std::string text) : text_(std::move(text)) {}

    void ComputeRequirement() override {
        requirement_.min_x = 1;
        requirement_.min_y = 1;
        requirement_.flex_grow_x = 1;
        requirement_.flex_shrink_x = 1;
    }

    void Render(ftxui::Screen& screen) override {
        const int width = box_.x_max - box_.x_min + 1;
        std::string shown = text_;
        if (ftxui::string_width(shown) > width) {
            // Quita glifos del principio hasta que "…" + el resto quepa.
            const std::vector<std::string> glyphs = ftxui::Utf8ToGlyphs(text_);
            std::size_t first = 0;
            std::string rest;
            do {
                rest.clear();
                for (std::size_t i = ++first; i < glyphs.size(); ++i) {
                    rest += glyphs[i];
                }
            } while (first < glyphs.size() && ftxui::string_width(rest) + 1 > width);
            shown = "…" + rest;
        }
        ftxui::Element line = ftxui::text(shown);
        line->ComputeRequirement();
        line->SetBox(box_);
        line->Render(screen);
    }

private:
    std::string text_;
};

/// Al menos rows filas (las de un frame, que pide todo su contenido) y lo
/// que sobre; el foco se ve dentro de esas filas.
class MinRows : public ftxui::Node {
public:
    MinRows(ftxui::Element child, int rows) : Node({std::move(child)}), rows_(rows) {}

    void ComputeRequirement() override {
        Node::ComputeRequirement();
        requirement_.min_y = rows_;
        requirement_.flex_grow_y = 1;
        requirement_.flex_shrink_y = 0;
        if (requirement_.focused.enabled) {
            requirement_.focused.box.y_min = 0;
            requirement_.focused.box.y_max = rows_ - 1;
        }
    }

    void SetBox(ftxui::Box box) override {
        Node::SetBox(box);
        children_[0]->SetBox(box);
    }

private:
    int rows_;
};

std::string with_tilde(const std::string& path, const std::string& home) {
    if (!home.empty() && path.size() > home.size() && path.compare(0, home.size(), home) == 0 &&
        path[home.size()] == '/') {
        return "~" + path.substr(home.size());
    }
    return path;
}

} // namespace

AddonsPanel::AddonsPanel(const Palette& palette, Notice notice, std::function<void()> retry_save)
    : palette_(palette), notice_(std::move(notice)), retry_save_(std::move(retry_save)),
      addon_(*settings::find_addon("telegram")) {
    for (std::size_t i = 0; i < kAddonFieldCount; ++i) {
        const auto field = static_cast<AddonField>(i);
        ftxui::InputOption option;
        option.multiline = false;
        option.transform = [this](ftxui::InputState state) {
            return input_transform(std::move(state.element), state.hovered, state.focused,
                                   state.is_placeholder, palette_);
        };
        ftxui::StringRef placeholder{""};
        switch (field) {
        case AddonField::Token:
            option.password = true; // Nunca se dibuja en claro.
            placeholder = &token_placeholder_;
            break;
        case AddonField::ModelKey:
            option.password = true;
            placeholder = &model_key_placeholder_;
            option.on_change = [this] {
                form_.remove_model_key = false; // Escribir otra deshace [Quitar].
                refresh_placeholders();
            };
            break;
        case AddonField::SearchKey:
            option.password = true;
            placeholder = &search_key_placeholder_;
            option.on_change = [this] {
                form_.remove_search_key = false;
                refresh_placeholders();
            };
            break;
        case AddonField::Model:
            placeholder = "Mismo que la app"; // Cuál, en la fila de abajo.
            break;
        case AddonField::TimeZone:
            placeholder = std::string{settings::default_time_zone()};
            break;
        default:
            break;
        }
        inputs_[i] = ftxui::Input(&form_.text[i], placeholder, option);
    }
    enabled_box_ = ftxui::Checkbox("Activado", &form_.enabled, check_option(palette_));
    details_box_ = ftxui::Checkbox("Detalle de errores en el registro", &form_.log_error_details,
                                   check_option(palette_));
    remove_model_key_ = ftxui::Button(
        "Quitar",
        [this] {
            form_.at(AddonField::ModelKey).clear();
            form_.remove_model_key = !secrets_.model_key.empty();
            refresh_placeholders();
        },
        button_option(palette_));
    remove_search_key_ = ftxui::Button(
        "Quitar",
        [this] {
            form_.at(AddonField::SearchKey).clear();
            form_.remove_search_key = !secrets_.search_key.empty();
            refresh_placeholders();
        },
        button_option(palette_));
    generate_code_ = ftxui::Button(
        "Generar",
        [this] {
            Result<std::string> code = settings::generate_invite_code();
            if (code.is_error()) {
                notice_(code.error().message);
                return;
            }
            form_.at(AddonField::InviteCode) = code.value();
        },
        button_option(palette_));
    // Sin Activado en lo guardado no hay qué reiniciar: atenuado (como
    // [Exportar] sin respuestas) y sin efecto.
    ftxui::ButtonOption restart_option = button_option(palette_);
    restart_option.transform = [this](const ftxui::EntryState& state) {
        const ftxui::Element element = button_label(state.label, state.focused, palette_);
        return saved_form_.enabled || state.focused
                   ? element
                   : element | palette_.ink(&Theme::input_placeholder);
    };
    restart_button_ = ftxui::Button(
        "Reiniciar",
        [this] {
            if (!worker_) {
                return;
            }
            if (!saved_form_.enabled) {
                notice_(std::string{kRestartNeedsEnabled});
                return;
            }
            notice_("Reiniciando el bot…");
            worker_->push([this, params = unit_params()](const CancelToken& cancel) {
                const ServiceAction action = control_->restart(&cancel);
                const ServiceStatus status = control_->status(params, &cancel);
                return std::function<void()>{[this, action, status] {
                    service_ = status;
                    notice_(action.message);
                }};
            });
        },
        std::move(restart_option));
    refresh_button_ =
        ftxui::Button("Actualizar estado", [this] { refresh_status(); }, button_option(palette_));
    ftxui::MenuOption invited = ftxui::MenuOption::Vertical();
    invited.entries = &invited_names_;
    invited.selected = &invited_selected_;
    invited.entries_option.transform = [this](const ftxui::EntryState& state) {
        return list_row(state.label, state.focused, false, palette_);
    };
    invited_menu_ = ftxui::Menu(invited);

    const auto editable = [this] { return available() && !read_only(); };
    ftxui::Components fields{enabled_box_};
    for (std::size_t i = 0; i < kAddonFieldCount; ++i) {
        const auto field = static_cast<AddonField>(i);
        if (field == AddonField::ModelKey) {
            fields.push_back(ftxui::Container::Horizontal({inputs_[i], remove_model_key_}));
        } else if (field == AddonField::SearchKey) {
            fields.push_back(ftxui::Container::Horizontal({inputs_[i], remove_search_key_}));
        } else if (field == AddonField::InviteCode) {
            fields.push_back(ftxui::Container::Horizontal({inputs_[i], generate_code_}));
        } else {
            fields.push_back(inputs_[i]);
        }
    }
    fields.push_back(details_box_);
    root_ = ftxui::Container::Vertical({
        ftxui::Maybe(ftxui::Container::Horizontal({restart_button_, refresh_button_}),
                     [this] { return available(); }),
        ftxui::Maybe(ftxui::Container::Vertical(std::move(fields)), editable),
        ftxui::Maybe(invited_menu_, [this] { return available() && !invited_names_.empty(); }),
    });
}

AddonsPanel::~AddonsPanel() = default;

void AddonsPanel::set_environment(AddonsEnvironment environment) {
    environment_ = std::move(environment);
    if (environment_->os == Os::Windows || !environment_->unit_path.has_value()) {
        return; // Sin procesos.
    }
    control_ = std::make_unique<ServiceControl>(addon_, *environment_->unit_path,
                                                environment_->uid, environment_->run);
    worker_ = std::make_unique<ServiceWorker>(environment_->post);
}

bool AddonsPanel::available() const {
    return environment_.has_value() && environment_->os != Os::Windows;
}

bool AddonsPanel::read_only() const {
    return !load_error_.empty() || read_.newer() ||
           (install_.has_value() && install_->path.has_value() &&
            !addon_install_problem(addon_, *install_, environment_->portable).empty());
}

settings::UnitParams AddonsPanel::unit_params() const {
    return settings::UnitParams{environment_->chatbot_path, std::string{addon_.name},
                                std::string{addon_.description}, environment_->xdg_config_home,
                                environment_->xdg_data_home};
}

void AddonsPanel::open(const std::string& app_model) {
    question_.reset();
    discard_confirmed_ = false;
    status_requested_ = false;
    install_.reset();
    service_.reset();
    load_error_.clear();
    read_ = {};
    secrets_ = {};
    if (!available()) {
        return;
    }
    app_model_ = clean(app_model);
    Result<settings::TelegramSectionRead> read =
        settings::read_telegram_section(environment_->config_path);
    if (read.is_error()) {
        load_error_ = read.error().message;
    } else {
        read_ = read.value();
    }
    Result<Credentials> credentials = load_credentials(environment_->credentials_path);
    if (credentials.is_error()) {
        load_error_ = credentials.error().message;
    } else {
        secrets_.token = settings::credential(credentials.value(), settings::kTokenCredentialsKey);
        secrets_.model_key =
            settings::credential(credentials.value(), settings::kBotApiKeyCredentialsKey);
        secrets_.search_key =
            settings::credential(credentials.value(), settings::kBotSearchKeyCredentialsKey);
    }
    form_ = form_from_section(read_.values);
    saved_form_ = form_;
    refresh_placeholders();

    invited_error_.clear();
    if (!environment_->data_dir.has_value()) {
        invited_error_ = "No se sabe dónde guarda sus datos el bot (falta HOME).";
        set_invited({});
        return;
    }
    const std::string path = settings::invited_file_path(*environment_->data_dir);
    Result<std::vector<std::int64_t>> invited = settings::load_invited(path);
    if (invited.is_error()) {
        // Dañado: lo mismo que dice el estado con la salida 65.
        invited_error_ = invited.error().kind == ErrorKind::BadResponse
                             ? exit_status_message(65, addon_.executable, *environment_->data_dir)
                             : invited.error().message;
        set_invited({});
        return;
    }
    set_invited(invited.value());
}

void AddonsPanel::show() {
    if (!status_requested_) {
        status_requested_ = true;
        refresh_status();
    }
}

void AddonsPanel::refresh_placeholders() {
    token_placeholder_ = addon_secret_status(secrets_.token, false);
    model_key_placeholder_ = addon_secret_status(secrets_.model_key, form_.remove_model_key);
    search_key_placeholder_ = addon_secret_status(secrets_.search_key, form_.remove_search_key);
}

void AddonsPanel::refresh_status() {
    if (!worker_) {
        return;
    }
    worker_->push([this, params = unit_params()](const CancelToken& cancel) {
        AddonInstall install;
        install.path = environment_->locate(addon_);
        if (install.path.has_value()) {
            ProcessOptions options;
            options.timeout = kServiceCommandTimeout;
            options.cancel = &cancel;
            install.config_version =
                parse_config_version(environment_->run({*install.path, "--config-version"},
                                                       options));
        }
        const ServiceStatus status = control_->status(params, &cancel);
        return std::function<void()>{[this, install, status] {
            install_ = install;
            service_ = status;
        }};
    });
}

void AddonsPanel::set_invited(std::vector<std::int64_t> users) {
    invited_ = std::move(users);
    invited_names_.clear();
    for (const std::int64_t user : invited_) {
        invited_names_.push_back(std::to_string(user));
    }
    invited_selected_ =
        std::clamp(invited_selected_, 0, std::max(0, static_cast<int>(invited_names_.size()) - 1));
}

bool AddonsPanel::dirty() const {
    return available() && !read_only() && form_ != saved_form_;
}

std::string AddonsPanel::out_of_sync() const {
    if (!available() || read_only() || !service_.has_value() || !install_.has_value() ||
        !install_->path.has_value()) {
        return {};
    }
    return service_mismatch(*service_, form_.enabled);
}

bool AddonsPanel::pending() const { return dirty() || !out_of_sync().empty(); }

std::optional<std::string> AddonsPanel::check() const {
    if (!pending()) {
        return std::nullopt;
    }
    return addon_form_error(form_, secrets_);
}

bool AddonsPanel::ask_before_saving() {
    if (!dirty() || read_.discarded.empty() || discard_confirmed_) {
        return false;
    }
    std::string keys;
    for (const std::string& key : read_.discarded) {
        keys += (keys.empty() ? "" : ", ") + clean(key);
    }
    question_ = Question{"Se descartarán: " + keys + ". ¿Guardar? (s/n)",
                         [this] {
                             discard_confirmed_ = true;
                             retry_save_();
                         },
                         {},
                         "Cancelado: no se guardó nada."};
    return true;
}

std::optional<std::string> AddonsPanel::commit() {
    if (!dirty()) {
        if (!out_of_sync().empty()) {
            apply_service(false, false); // Nada que escribir: solo el servicio.
        }
        return std::nullopt;
    }
    // Primero los secretos: si fallan, la sección no apunta a un token que no está.
    const std::vector<CredentialChange> changes = addon_credential_changes(form_);
    if (!changes.empty()) {
        Result<Credentials> loaded = load_credentials(environment_->credentials_path);
        if (loaded.is_error()) {
            return loaded.error().message;
        }
        Credentials credentials = loaded.value();
        for (const auto& [key, value] : changes) {
            if (value.has_value()) {
                credentials.keys[key] = *value;
            } else {
                credentials.keys.erase(key);
            }
        }
        if (const std::optional<ChatError> error =
                save_credentials(environment_->credentials_path, credentials)) {
            return error->message;
        }
    }
    if (const std::optional<ChatError> error =
            settings::write_telegram_section(environment_->config_path, section_from_form(form_))) {
        return error->message;
    }
    const bool restart = addon_needs_restart(saved_form_, secrets_, form_) && form_.enabled &&
                         service_.has_value() && service_->active == "active";
    // Lo escrito pasa a ser lo guardado.
    if (!form_.at(AddonField::Token).empty()) {
        secrets_.token = form_.at(AddonField::Token);
    }
    if (form_.remove_model_key) {
        secrets_.model_key.clear();
    } else if (!form_.at(AddonField::ModelKey).empty()) {
        secrets_.model_key = form_.at(AddonField::ModelKey);
    }
    if (form_.remove_search_key) {
        secrets_.search_key.clear();
    } else if (!form_.at(AddonField::SearchKey).empty()) {
        secrets_.search_key = form_.at(AddonField::SearchKey);
    }
    for (const AddonField field : {AddonField::Token, AddonField::ModelKey, AddonField::SearchKey}) {
        form_.at(field).clear();
    }
    form_.remove_model_key = false;
    form_.remove_search_key = false;
    form_ = form_from_section(section_from_form(form_)); // Como se lee al abrir.
    saved_form_ = form_;
    read_.discarded.clear();
    read_.version = settings::kTelegramConfigVersion;
    refresh_placeholders();

    if (restart) {
        question_ = Question{"Este cambio necesita reiniciar el bot y se borran las "
                             "conversaciones en memoria. ¿Reiniciar ahora? (s/n)",
                             [this] { apply_service(true, false); },
                             [this] { apply_service(false, false); },
                             "Cancelado: se guardó, pero no se tocó el servicio."};
    } else {
        apply_service(false, false);
    }
    return std::nullopt;
}

void AddonsPanel::apply_service(bool restart, bool replace_foreign) {
    if (!worker_) {
        notice_("Guardado.");
        return;
    }
    // Lo que dice la casilla, no si cambió: apply mira cómo está el servicio ahora.
    const bool enabled = form_.enabled;
    notice_(enabled ? "Guardado. Aplicando al servicio…" : "Guardado. Deteniendo el servicio…");
    worker_->push([this, enabled, restart, replace_foreign,
                   params = unit_params()](const CancelToken& cancel) -> std::function<void()> {
        ServiceAction action;
        std::string skipped;
        if (enabled && !environment_->locate(addon_).has_value()) {
            skipped = addon_install_problem(addon_, AddonInstall{}, environment_->portable);
        } else {
            action = control_->apply(params, enabled, replace_foreign, restart, &cancel);
        }
        const ServiceStatus status = control_->status(params, &cancel);
        return [this, action, skipped, status, restart] {
            service_ = status;
            if (action.kind == ServiceAction::Kind::Confirm) {
                question_ = Question{
                    action.message, [this, restart] { apply_service(restart, true); },
                    [this] { notice_("Guardado. No se tocó el servicio existente."); },
                    "Cancelado: se guardó, pero no se tocó el servicio existente."};
                return;
            }
            const std::string& detail = skipped.empty() ? action.message : skipped;
            notice_(detail.empty() ? "Guardado." : "Guardado. " + detail);
        };
    });
}

void AddonsPanel::answer(bool yes) {
    if (!question_) {
        return;
    }
    const Question question = std::move(*question_);
    question_.reset();
    const std::function<void()>& next = yes ? question.yes : question.no;
    if (next) {
        next();
    }
}

void AddonsPanel::cancel_question() {
    if (question_) {
        notice_(question_->cancelled);
        question_.reset();
    }
}

void AddonsPanel::ask_remove_invited() {
    if (invited_names_.empty() || !environment_->data_dir.has_value()) {
        return;
    }
    const std::int64_t user = invited_[static_cast<std::size_t>(invited_selected_)];
    question_ = Question{
        "¿Quitar " + std::to_string(user) + " ahora? No espera a Guardar. (s/n)",
        [this, user] {
            Result<std::vector<std::int64_t>> left = settings::remove_invited(
                settings::invited_file_path(*environment_->data_dir), user);
            if (left.is_error()) {
                notice_(left.error().message);
                return;
            }
            set_invited(left.value());
            const std::string done = "Se quitó " + std::to_string(user) + ".";
            if (!worker_ || !service_.has_value() || service_->active != "active") {
                notice_(done);
                return;
            }
            // Si no, el bot lo dejaría entrar hasta reiniciar.
            worker_->push([this, done](const CancelToken& cancel) {
                const ServiceAction action = control_->reload_if_active(&cancel);
                return std::function<void()>{
                    [this, done, action] { notice_(done + " " + action.message); }};
            });
        },
        {},
        "Cancelado: no se quitó a nadie."};
}

std::vector<ftxui::Component> AddonsPanel::focus_order() const {
    std::vector<ftxui::Component> order;
    if (!available()) {
        return order;
    }
    order.push_back(restart_button_);
    order.push_back(refresh_button_);
    if (!read_only()) {
        order.push_back(enabled_box_);
        for (std::size_t i = 0; i < kAddonFieldCount; ++i) {
            order.push_back(inputs_[i]);
            const auto field = static_cast<AddonField>(i);
            if (field == AddonField::ModelKey) {
                order.push_back(remove_model_key_);
            } else if (field == AddonField::SearchKey) {
                order.push_back(remove_search_key_);
            } else if (field == AddonField::InviteCode) {
                order.push_back(generate_code_);
            }
        }
        order.push_back(details_box_);
    }
    if (!invited_names_.empty()) {
        order.push_back(invited_menu_);
    }
    return order;
}

bool AddonsPanel::handle_event(const ftxui::Event& event) {
    if (available() && event == ftxui::Event::Delete && invited_menu_->Focused()) {
        ask_remove_invited();
        return true;
    }
    return false;
}

bool AddonsPanel::text_field_focused() const {
    return std::any_of(inputs_.begin(), inputs_.end(),
                       [](const ftxui::Component& input) { return input->Focused(); });
}

ftxui::Element AddonsPanel::render_block() const {
    const ftxui::Decorator notice = palette_.ink(&Theme::notice);
    const ftxui::Decorator error = palette_.ink(&Theme::error);
    ftxui::Elements rows;
    if (!install_.has_value()) {
        rows.push_back(ftxui::text("Buscando " + std::string{addon_.executable} + "…") | notice);
    } else if (const std::string problem =
                   addon_install_problem(addon_, *install_, environment_->portable);
               !problem.empty()) {
        rows.push_back(ftxui::paragraph(clean(problem)) | error);
    } else {
        rows.push_back(ftxui::hbox(
            {ftxui::text("Instalado: "),
             std::make_shared<TailText>(clean(with_tilde(*install_->path, environment_->home)))}));
    }
    if (!control_) {
        rows.push_back(ftxui::text("Servicio: sin HOME no hay dónde instalarlo") | notice);
    } else if (!service_.has_value()) {
        rows.push_back(ftxui::text("Servicio: consultando…") | notice);
    } else {
        rows.push_back(ftxui::text(clean(service_summary(*service_))));
        if (const std::string mismatch = out_of_sync(); !mismatch.empty()) {
            rows.push_back(ftxui::paragraph(mismatch) | error);
        }
        const std::string problem = service_problem(*service_, addon_.executable,
                                                    environment_->data_dir.value_or(""));
        if (!problem.empty()) {
            rows.push_back(ftxui::paragraph(clean(problem)) | error);
        }
    }
    rows.push_back(ftxui::hbox(
        {restart_button_->Render(), ftxui::text(" "), refresh_button_->Render()}));
    if (service_.has_value() && service_->linger == false) {
        rows.push_back(ftxui::text(std::string{palette_.glyph(Glyph::Warning)} +
                                   " Sin linger: " + std::string{kLingerCommand}) |
                       error);
    }
    if (control_) {
        rows.push_back(ftxui::text("Registro: " + journal_command(addon_)) | notice);
    }
    // Lo de abajo (el registro, después el linger) es lo primero que se pierde.
    return ftxui::vbox(std::move(rows)) | ftxui::yframe |
           ftxui::size(ftxui::HEIGHT, ftxui::LESS_THAN, kBlockRows + 1);
}

ftxui::Element AddonsPanel::render_field(AddonField field) const {
    const std::size_t index = static_cast<std::size_t>(field);
    const ftxui::Component& input = inputs_[index];
    ftxui::Element value;
    if (read_only()) {
        // Los secretos, solo su estado.
        const bool secret = field == AddonField::Token || field == AddonField::ModelKey ||
                            field == AddonField::SearchKey;
        const std::string& placeholder = field == AddonField::Token      ? token_placeholder_
                                         : field == AddonField::ModelKey ? model_key_placeholder_
                                                                         : search_key_placeholder_;
        value = ftxui::text(clean(secret ? placeholder : form_.at(field))) |
                palette_.ink(&Theme::notice);
    } else {
        value = input->Render() | ftxui::flex;
        if (field == AddonField::ModelKey) {
            value = ftxui::hbox({value, ftxui::text(" "), remove_model_key_->Render()});
        } else if (field == AddonField::SearchKey) {
            value = ftxui::hbox({value, ftxui::text(" "), remove_search_key_->Render()});
        } else if (field == AddonField::InviteCode) {
            value = ftxui::hbox({value, ftxui::text(" "), generate_code_->Render()});
        }
    }
    ftxui::Elements rows{ftxui::hbox(
        {label(addon_field_label(field), input->Focused(), palette_), value | ftxui::flex})};
    if (!read_only()) {
        if (const std::optional<std::string> error = addon_field_error(form_, secrets_, field)) {
            rows.push_back(indented_note(clean(*error)) | palette_.ink(&Theme::error));
        }
    }
    return ftxui::vbox(std::move(rows));
}

ftxui::Element AddonsPanel::render_invited() const {
    const ftxui::Decorator notice = palette_.ink(&Theme::notice);
    ftxui::Elements rows{
        ftxui::text("Invitados" + (invited_error_.empty()
                                       ? " (" + std::to_string(invited_.size()) + ")"
                                       : std::string{})) |
        ftxui::bold};
    if (!invited_error_.empty()) {
        rows.push_back(ftxui::paragraph(clean(invited_error_)) | palette_.ink(&Theme::error));
    } else if (invited_.empty()) {
        rows.push_back(ftxui::text("  Nadie ha entrado con el código.") | notice);
    } else {
        rows.push_back(invited_menu_->Render());
        rows.push_back(ftxui::text("  Supr: quitar (al momento)") | notice);
    }
    return ftxui::vbox(std::move(rows));
}

ftxui::Element AddonsPanel::render_fields() const {
    const ftxui::Decorator notice = palette_.ink(&Theme::notice);
    ftxui::Elements rows;
    if (service_.has_value() && service_->linger == false) {
        rows.push_back(ftxui::paragraph(std::string{kNoLinger}) | notice);
    }
    if (const std::string note = addon_env_note(environment_->env_defined); !note.empty()) {
        rows.push_back(ftxui::paragraph(note) | notice);
    }
    if (!load_error_.empty()) {
        rows.push_back(ftxui::paragraph(clean(load_error_) + " Solo lectura.") |
                       palette_.ink(&Theme::error));
    } else if (read_.newer()) {
        rows.push_back(ftxui::paragraph("La sección es de una versión más nueva (" +
                                        std::to_string(*read_.version) +
                                        ") de la configuración: solo lectura.") |
                       palette_.ink(&Theme::error));
    }
    rows.push_back(read_only()
                       ? ftxui::text(std::string{form_.enabled ? "[x]" : "[ ]"} + " Activado") |
                             notice
                       : enabled_box_->Render());
    for (std::size_t i = 0; i < kAddonFieldCount; ++i) {
        const auto field = static_cast<AddonField>(i);
        rows.push_back(render_field(field));
        if (field == AddonField::Model && !app_model_.empty()) {
            rows.push_back(indented_note("La app usa " + app_model_ + ".") | notice);
        }
        if (field == AddonField::SearchKey) {
            rows.push_back(indented_note("Sin key propia se usan las de la app. Recomendado: "
                                         "una propia con tope diario en el proveedor.") |
                           notice);
        }
    }
    rows.push_back(read_only() ? ftxui::text(std::string{form_.log_error_details ? "[x]" : "[ ]"} +
                                             " Detalle de errores en el registro") |
                                     notice
                               : details_box_->Render());
    rows.push_back(indented_note("Puede incluir texto de los alumnos: solo para diagnosticar.") |
                   notice);
    rows.push_back(ftxui::separator() | palette_.ink(&Theme::border));
    rows.push_back(render_invited());
    return ftxui::vbox(std::move(rows));
}

ftxui::Element AddonsPanel::render() const {
    ftxui::Elements page{ftxui::text(std::string{addon_.title}) | ftxui::bold};
    if (!available()) {
        page.push_back(ftxui::text(""));
        page.push_back(ftxui::paragraph(std::string{settings::kAddonsUnavailableOnWindows}) |
                       palette_.ink(&Theme::notice));
        page.push_back(ftxui::filler());
        return ftxui::vbox(std::move(page));
    }
    page.push_back(render_block());
    page.push_back(ftxui::separator() | palette_.ink(&Theme::border));
    // Los campos se desplazan solos; si no les quedan kMinFieldRows filas,
    // se desplaza todo (el frame de afuera).
    page.push_back(std::make_shared<MinRows>(palette_.vscroll(render_fields()) | ftxui::yframe,
                                             kMinFieldRows));
    return ftxui::vbox(std::move(page)) | ftxui::yframe;
}

} // namespace chatbot::cli
