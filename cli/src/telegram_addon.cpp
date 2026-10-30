#include "telegram_addon.h"

#include "chatbot/addon_settings/invited.h"
#include "chatbot/addon_settings/paths.h"
#include "chatbot/addon_settings/secrets.h"
#include "chatbot/addon_settings/token.h"
#include "chatbot/credentials.h"
#include "chatbot/markdown/markdown.h"

#include <algorithm>
#include <utility>

namespace chatbot::cli {
namespace {

namespace settings = addon_settings;

} // namespace

TelegramAddon::TelegramAddon(AddonsEnvironment environment)
    : addon_(*settings::find_addon("telegram")), environment_(std::move(environment)) {
    if (!available()) {
        return; // Sin procesos.
    }
    if (environment_.unit_path.has_value()) {
        control_ = std::make_unique<ServiceControl>(addon_, *environment_.unit_path,
                                                    environment_.uid, environment_.run);
    }
    worker_ = std::make_unique<ServiceWorker>(environment_.post);
}

TelegramAddon::~TelegramAddon() = default;

settings::UnitParams TelegramAddon::unit_params() const {
    return settings::UnitParams{environment_.chatbot_path, std::string{addon_.name},
                                std::string{addon_.description}, environment_.xdg_config_home,
                                environment_.xdg_data_home};
}

void TelegramAddon::forget_status() {
    install_.reset();
    service_.reset();
}

void TelegramAddon::load() {
    load_error_.clear();
    read_ = {};
    secrets_ = {};
    saved_form_ = {};
    invited_.clear();
    invited_error_.clear();
    if (!available()) {
        return;
    }
    Result<settings::TelegramSectionRead> read =
        settings::read_telegram_section(environment_.config_path);
    if (read.is_error()) {
        load_error_ = read.error().message;
    } else {
        read_ = read.value();
    }
    Result<Credentials> credentials = load_credentials(environment_.credentials_path);
    if (credentials.is_error()) {
        load_error_ = credentials.error().message;
    } else {
        secrets_.token = settings::credential(credentials.value(), settings::kTokenCredentialsKey);
        secrets_.model_key =
            settings::credential(credentials.value(), settings::kBotApiKeyCredentialsKey);
        secrets_.search_key =
            settings::credential(credentials.value(), settings::kBotSearchKeyCredentialsKey);
    }
    saved_form_ = form_from_section(read_.values);

    if (!environment_.data_dir.has_value()) {
        invited_error_ = "No se sabe dónde guarda sus datos el bot (falta HOME).";
        return;
    }
    Result<std::vector<std::int64_t>> invited =
        settings::load_invited(settings::invited_file_path(*environment_.data_dir));
    if (invited.is_error()) {
        // Dañado: lo mismo que dice el estado con la salida 65.
        invited_error_ = invited.error().kind == ErrorKind::BadResponse
                             ? exit_status_message(65, addon_.executable, *environment_.data_dir)
                             : invited.error().message;
        return;
    }
    invited_ = invited.value();
}

bool TelegramAddon::read_only() const {
    return !load_error_.empty() || read_.newer() ||
           (install_.has_value() && install_->path.has_value() &&
            !addon_install_problem(addon_, *install_, environment_.portable).empty());
}

std::string TelegramAddon::section_problem() const {
    if (!load_error_.empty()) {
        return load_error_ + " Solo lectura.";
    }
    if (read_.newer()) {
        return "La sección es de una versión más nueva (" + std::to_string(*read_.version) +
               ") de la configuración: solo lectura.";
    }
    return {};
}

std::string TelegramAddon::discard_question() const {
    if (read_.discarded.empty()) {
        return {};
    }
    std::string keys;
    for (const std::string& key : read_.discarded) {
        std::string clean = md::sanitize(key);
        std::replace(clean.begin(), clean.end(), '\n', ' ');
        keys += (keys.empty() ? "" : ", ") + clean;
    }
    return "Se descartarán: " + keys + ". ¿Guardar? (s/n)";
}

std::string TelegramAddon::out_of_sync(bool enabled) const {
    if (!available() || read_only() || !service_.has_value() || !install_.has_value() ||
        !install_->path.has_value()) {
        return {};
    }
    return service_mismatch(*service_, enabled);
}

bool TelegramAddon::needs_restart(const AddonForm& form) const {
    return addon_needs_restart(saved_form_, secrets_, form) && form.enabled &&
           service_.has_value() && service_->active == "active";
}

std::optional<std::string> TelegramAddon::save(const AddonForm& form) {
    // Primero los secretos: si fallan, la sección no apunta a un token que no está.
    const std::vector<CredentialChange> changes = addon_credential_changes(form);
    if (!changes.empty()) {
        Result<Credentials> loaded = load_credentials(environment_.credentials_path);
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
                save_credentials(environment_.credentials_path, credentials)) {
            return error->message;
        }
    }
    const settings::TelegramSection section = section_from_form(form);
    if (const std::optional<ChatError> error =
            settings::write_telegram_section(environment_.config_path, section)) {
        return error->message;
    }
    // Lo escrito pasa a ser lo guardado.
    if (!form.at(AddonField::Token).empty()) {
        secrets_.token = form.at(AddonField::Token);
    }
    if (form.remove_model_key) {
        secrets_.model_key.clear();
    } else if (!form.at(AddonField::ModelKey).empty()) {
        secrets_.model_key = form.at(AddonField::ModelKey);
    }
    if (form.remove_search_key) {
        secrets_.search_key.clear();
    } else if (!form.at(AddonField::SearchKey).empty()) {
        secrets_.search_key = form.at(AddonField::SearchKey);
    }
    saved_form_ = form_from_section(section); // Como se lee al abrir.
    read_.discarded.clear();
    read_.version = settings::kTelegramConfigVersion;
    return std::nullopt;
}

Result<std::string> TelegramAddon::invite_code() { return settings::generate_invite_code(); }

void TelegramAddon::remove_invited(std::int64_t user, std::function<void(const Removal&)> done) {
    if (!environment_.data_dir.has_value()) {
        done({"No se sabe dónde guarda sus datos el bot (falta HOME).", {}});
        return;
    }
    Result<std::vector<std::int64_t>> left =
        settings::remove_invited(settings::invited_file_path(*environment_.data_dir), user);
    if (left.is_error()) {
        done({left.error().message, {}});
        return;
    }
    invited_ = left.value();
    std::string message = "Se quitó " + std::to_string(user) + ".";
    if (!control_ || !service_.has_value() || service_->active != "active") {
        done({std::nullopt, std::move(message)});
        return;
    }
    // Si no, el bot lo dejaría entrar hasta reiniciar.
    worker_->push([this, message = std::move(message),
                   done = std::move(done)](const CancelToken& cancel) {
        const ServiceAction action = control_->reload_if_active(&cancel);
        return std::function<void()>{[message, done, action] {
            done({std::nullopt, message + " " + action.message});
        }};
    });
}

AddonInstall TelegramAddon::find_install(const CancelToken& cancel) const {
    AddonInstall install;
    install.path = environment_.locate(addon_);
    if (install.path.has_value()) {
        ProcessOptions options;
        options.timeout = kServiceCommandTimeout;
        options.cancel = &cancel;
        install.config_version =
            parse_config_version(environment_.run({*install.path, "--config-version"}, options));
    }
    return install;
}

void TelegramAddon::check_install(std::function<void()> done) {
    worker_->push([this, done = std::move(done)](const CancelToken& cancel) {
        const AddonInstall install = find_install(cancel);
        return std::function<void()>{[this, install, done] {
            install_ = install;
            done();
        }};
    });
}

void TelegramAddon::refresh(std::function<void()> done) {
    worker_->push([this, params = unit_params(),
                   done = std::move(done)](const CancelToken& cancel) {
        const AddonInstall install = find_install(cancel);
        const ServiceStatus status = control_->status(params, &cancel);
        return std::function<void()>{[this, install, status, done] {
            install_ = install;
            service_ = status;
            done();
        }};
    });
}

void TelegramAddon::apply(bool restart, bool replace_foreign,
                          std::function<void(const Applied&)> done) {
    // Lo guardado, no si cambió: apply mira cómo está el servicio ahora.
    const bool enabled = saved_form_.enabled;
    worker_->push([this, enabled, restart, replace_foreign, params = unit_params(),
                   done = std::move(done)](const CancelToken& cancel) {
        Applied applied;
        if (enabled && !environment_.locate(addon_).has_value()) {
            applied.skipped = addon_install_problem(addon_, AddonInstall{}, environment_.portable);
        } else {
            applied.action = control_->apply(params, enabled, replace_foreign, restart, &cancel);
        }
        const ServiceStatus status = control_->status(params, &cancel);
        return std::function<void()>{[this, applied, status, done] {
            service_ = status;
            done(applied);
        }};
    });
}

void TelegramAddon::restart(std::function<void(const ServiceAction&)> done) {
    worker_->push([this, params = unit_params(),
                   done = std::move(done)](const CancelToken& cancel) {
        const ServiceAction action = control_->restart(&cancel);
        const ServiceStatus status = control_->status(params, &cancel);
        return std::function<void()>{[this, action, status, done] {
            service_ = status;
            done(action);
        }};
    });
}

void TelegramAddon::disable(std::function<void(const ServiceAction&)> done) {
    worker_->push([this, params = unit_params(),
                   done = std::move(done)](const CancelToken& cancel) {
        const ServiceAction action = control_->disable(&cancel);
        const ServiceStatus status = control_->status(params, &cancel);
        return std::function<void()>{[this, action, status, done] {
            service_ = status;
            done(action);
        }};
    });
}

void TelegramAddon::reload(std::function<void(const ServiceAction&)> done) {
    worker_->push([this, done = std::move(done)](const CancelToken& cancel) {
        const ServiceAction action = control_->reload_if_active(&cancel);
        return std::function<void()>{[action, done] { done(action); }};
    });
}

} // namespace chatbot::cli
