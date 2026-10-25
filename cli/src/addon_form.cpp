#include "addon_form.h"

#include "provider_settings.h"

#include "chatbot/addon_settings/limits.h"
#include "chatbot/addon_settings/secrets.h"
#include "chatbot/addon_settings/time_zone.h"
#include "chatbot/addon_settings/token.h"

#include <charconv>

namespace chatbot::cli {
namespace {

namespace settings = addon_settings;

std::string_view trim(std::string_view text) {
    const std::size_t begin = text.find_first_not_of(" \t");
    if (begin == std::string_view::npos) {
        return {};
    }
    return text.substr(begin, text.find_last_not_of(" \t") - begin + 1);
}

/// Un entero decimal (con espacios alrededor), o nullopt.
std::optional<std::int64_t> parse_integer(std::string_view text) {
    text = trim(text);
    std::int64_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || error != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

/// "1, 2,3" → {1, 2, 3}; vacío → {}; otra cosa → nullopt.
std::optional<std::vector<std::int64_t>> parse_ids(std::string_view text) {
    std::vector<std::int64_t> ids;
    if (trim(text).empty()) {
        return ids;
    }
    while (true) {
        const std::size_t comma = text.find(',');
        const std::optional<std::int64_t> id = parse_integer(text.substr(0, comma));
        if (!id.has_value()) {
            return std::nullopt;
        }
        ids.push_back(*id);
        if (comma == std::string_view::npos) {
            return ids;
        }
        text.remove_prefix(comma + 1);
    }
}

std::string join_ids(const std::vector<std::int64_t>& ids) {
    std::string out;
    for (const std::int64_t id : ids) {
        out += (out.empty() ? "" : ", ") + std::to_string(id);
    }
    return out;
}

constexpr std::string_view kNotANumber = "Escribe un número entero.";

std::optional<std::string> number_error(const std::string& text,
                                        std::optional<std::string> (*check)(std::int64_t)) {
    const std::optional<std::int64_t> value = parse_integer(text);
    return value.has_value() ? check(*value) : std::string{kNotANumber};
}

std::int64_t number_or(const std::string& text, std::int64_t fallback) {
    return parse_integer(text).value_or(fallback);
}

} // namespace

std::string_view addon_field_label(AddonField field) {
    switch (field) {
    case AddonField::Token:
        return "Token";
    case AddonField::Model:
        return "Modelo";
    case AddonField::ModelKey:
        return "Key del modelo";
    case AddonField::SearchKey:
        return "Key de búsqueda";
    case AddonField::AllowedUsers:
        return "IDs autorizados";
    case AddonField::InviteCode:
        return "Código de invitación";
    case AddonField::DailyLimit:
        return "Mensajes por día";
    case AddonField::Concurrent:
        return "Respuestas a la vez";
    case AddonField::IdleMinutes:
        return "Min. de inactividad";
    case AddonField::StaleMinutes:
        return "Min. mensaje viejo";
    case AddonField::DraftMs:
        return "Vista previa (ms)";
    case AddonField::TimeZone:
        return "Zona horaria";
    case AddonField::Count:
        break;
    }
    return {};
}

AddonForm form_from_section(const settings::TelegramSection& section) {
    AddonForm form;
    form.enabled = section.enabled;
    form.log_error_details = section.log_error_details;
    form.at(AddonField::Model) = section.model;
    form.at(AddonField::AllowedUsers) = join_ids(section.allowed_users);
    form.at(AddonField::InviteCode) = section.invite_code;
    form.at(AddonField::DailyLimit) = std::to_string(section.daily_message_limit);
    form.at(AddonField::Concurrent) = std::to_string(section.max_concurrent_responses);
    form.at(AddonField::IdleMinutes) = std::to_string(section.session_idle_minutes);
    form.at(AddonField::StaleMinutes) = std::to_string(section.stale_message_minutes);
    form.at(AddonField::DraftMs) = std::to_string(section.draft_interval_ms);
    form.at(AddonField::TimeZone) = section.time_zone;
    return form;
}

std::optional<std::string> addon_field_error(const AddonForm& form, const AddonSecrets& saved,
                                             AddonField field) {
    const std::string& text = form.at(field);
    switch (field) {
    case AddonField::Token:
        if (!text.empty() && !settings::plausible_token(text)) {
            return "No parece un token de @BotFather (números, «:» y letras).";
        }
        if (form.enabled && text.empty() && saved.token.empty()) {
            return "Falta el token de @BotFather.";
        }
        return std::nullopt;
    case AddonField::ModelKey:
    case AddonField::SearchKey:
        if (!text.empty() && !plausible_key(text)) {
            return "No parece una key completa; pega la key completa.";
        }
        return std::nullopt;
    case AddonField::AllowedUsers: {
        const std::optional<std::vector<std::int64_t>> ids = parse_ids(text);
        if (!ids.has_value()) {
            return "Escribe IDs numéricos separados por comas.";
        }
        return settings::check_allowed_users(*ids);
    }
    case AddonField::InviteCode:
        return settings::check_invite_code(text);
    case AddonField::DailyLimit:
        return number_error(text, settings::check_daily_limit);
    case AddonField::Concurrent:
        return number_error(text, settings::check_concurrent_responses);
    case AddonField::IdleMinutes:
        return number_error(text, settings::check_idle_minutes);
    case AddonField::StaleMinutes:
        return number_error(text, settings::check_stale_minutes);
    case AddonField::DraftMs:
        return number_error(text, settings::check_draft_interval_ms);
    case AddonField::TimeZone:
        if (trim(text).empty()) {
            return std::nullopt; // La predeterminada.
        }
        if (Result<settings::DayFunction> day = settings::day_in_zone(trim(text));
            day.is_error()) {
            return day.error().message;
        }
        return std::nullopt;
    case AddonField::Model:
    case AddonField::Count:
        break;
    }
    return std::nullopt;
}

std::optional<std::string> addon_form_error(const AddonForm& form, const AddonSecrets& saved) {
    for (std::size_t i = 0; i < kAddonFieldCount; ++i) {
        const auto field = static_cast<AddonField>(i);
        if (const std::optional<std::string> error = addon_field_error(form, saved, field)) {
            return std::string{addon_field_label(field)} + ": " + *error;
        }
    }
    return std::nullopt;
}

settings::TelegramSection section_from_form(const AddonForm& form) {
    const settings::TelegramSection defaults;
    settings::TelegramSection section;
    section.enabled = form.enabled;
    section.log_error_details = form.log_error_details;
    section.model = std::string{trim(form.at(AddonField::Model))};
    section.allowed_users = parse_ids(form.at(AddonField::AllowedUsers)).value_or(
        std::vector<std::int64_t>{});
    section.invite_code = form.at(AddonField::InviteCode);
    section.daily_message_limit =
        number_or(form.at(AddonField::DailyLimit), defaults.daily_message_limit);
    section.max_concurrent_responses =
        number_or(form.at(AddonField::Concurrent), defaults.max_concurrent_responses);
    section.session_idle_minutes =
        number_or(form.at(AddonField::IdleMinutes), defaults.session_idle_minutes);
    section.stale_message_minutes =
        number_or(form.at(AddonField::StaleMinutes), defaults.stale_message_minutes);
    section.draft_interval_ms = number_or(form.at(AddonField::DraftMs), defaults.draft_interval_ms);
    section.time_zone = std::string{trim(form.at(AddonField::TimeZone))};
    return section;
}

std::vector<CredentialChange> addon_credential_changes(const AddonForm& form) {
    std::vector<CredentialChange> changes;
    if (!form.at(AddonField::Token).empty()) {
        changes.emplace_back(settings::kTokenCredentialsKey, form.at(AddonField::Token));
    }
    const auto secret = [&](AddonField field, bool removed, std::string_view key) {
        if (removed) {
            changes.emplace_back(key, std::nullopt);
        } else if (!form.at(field).empty()) {
            changes.emplace_back(key, form.at(field));
        }
    };
    secret(AddonField::ModelKey, form.remove_model_key, settings::kBotApiKeyCredentialsKey);
    secret(AddonField::SearchKey, form.remove_search_key, settings::kBotSearchKeyCredentialsKey);
    return changes;
}

bool addon_needs_restart(const AddonForm& saved_form, const AddonSecrets& saved,
                         const AddonForm& form) {
    const std::string& token = form.at(AddonField::Token);
    return (!token.empty() && token != saved.token) ||
           parse_integer(form.at(AddonField::Concurrent)) !=
               parse_integer(saved_form.at(AddonField::Concurrent));
}

std::string addon_secret_status(std::string_view saved, bool removed) {
    if (removed) {
        return "se quita: se usará la de la app";
    }
    return saved.empty() ? "sin configurar" : "guardada: " + mask_key(saved);
}

std::string addon_env_note(const std::vector<std::string_view>& defined) {
    if (defined.empty()) {
        return {};
    }
    std::string names;
    for (std::size_t i = 0; i < defined.size(); ++i) {
        names += (i == 0 ? "" : i + 1 == defined.size() ? " y " : ", ") + std::string{defined[i]};
    }
    return defined.size() == 1
               ? names + " está definida en esta terminal, pero el servicio no la ve."
               : names + " están definidas en esta terminal, pero el servicio no las ve.";
}

std::string addon_install_problem(const settings::AddonInfo& addon, const AddonInstall& install,
                                  bool portable) {
    const std::string executable{addon.executable};
    if (!install.path.has_value()) {
        return "No instalado: copia " + executable +
               (portable ? " junto a chatbot." : " junto a chatbot o en el PATH.");
    }
    if (!install.config_version.has_value()) {
        return "Incompatible: " + executable +
               " no dijo qué versión de la configuración entiende.";
    }
    if (*install.config_version != addon.config_version) {
        return "Incompatible: esta app usa la versión " + std::to_string(addon.config_version) +
               " de la configuración y " + executable + " entiende la " +
               std::to_string(*install.config_version) + ".";
    }
    return {};
}

std::optional<int> parse_config_version(const ProcessResult& result) {
    if (!result.ok()) {
        return std::nullopt;
    }
    std::string_view out = result.out;
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) {
        out.remove_suffix(1);
    }
    int value = 0;
    const auto [end, error] = std::from_chars(out.data(), out.data() + out.size(), value);
    if (out.empty() || error != std::errc{} || end != out.data() + out.size()) {
        return std::nullopt;
    }
    return value;
}

std::string service_summary(const ServiceStatus& status) {
    if (!status.systemd) {
        return std::string{kNoUserSystemd};
    }
    if (status.unit == settings::UnitState::Missing &&
        (status.enabled.empty() || status.enabled == "not-found")) {
        return "Servicio: no instalado";
    }
    const auto spanish = [](const std::string& word,
                            std::initializer_list<std::pair<std::string_view, std::string_view>>
                                table) {
        for (const auto& [from, to] : table) {
            if (word == from) {
                return std::string{to};
            }
        }
        return word.empty() ? std::string{"?"} : word;
    };
    std::string summary =
        "Servicio: " +
        spanish(status.active, {{"active", "activo"},
                                {"inactive", "detenido"},
                                {"failed", "falló"},
                                {"activating", "arrancando"},
                                {"deactivating", "deteniéndose"},
                                {"reloading", "recargando"}}) +
        " · " +
        spanish(status.enabled, {{"enabled", "habilitado"},
                                 {"disabled", "deshabilitado"},
                                 {"masked", "bloqueado"},
                                 {"not-found", "sin unidad"}});
    if (status.unit == settings::UnitState::Foreign) {
        summary += " · hecho a mano";
    }
    return summary;
}

std::string service_problem(const ServiceStatus& status, std::string_view executable,
                            std::string_view data_dir) {
    // Corriendo, un error viejo del registro ya no importa.
    if (!status.systemd || status.active == "active") {
        return {};
    }
    if (status.exit_status.has_value()) {
        std::string message = exit_status_message(*status.exit_status, executable, data_dir);
        if (!message.empty()) {
            return message;
        }
    }
    if (!status.last_error.empty()) {
        return "Último error: " + status.last_error;
    }
    if (status.exit_status.has_value()) {
        return "Salió con el código " + std::to_string(*status.exit_status) + ".";
    }
    return {};
}

std::string service_mismatch(const ServiceStatus& status, bool enabled) {
    if (!status.systemd) {
        return {};
    }
    const bool running = service_running(status.active);
    const bool is_enabled = status.enabled == "enabled";
    if (!enabled) {
        if (running) {
            return "Desactivado, pero el servicio sigue corriendo: guarda para detenerlo.";
        }
        return is_enabled ? "Desactivado, pero el servicio sigue habilitado: guarda para "
                            "deshabilitarlo."
                          : "";
    }
    switch (status.unit) {
    case settings::UnitState::Missing:
        return "Activado, pero el servicio no está instalado: guarda para instalarlo.";
    case settings::UnitState::Foreign:
        return "Activado, pero el servicio está hecho a mano: guarda para reemplazarlo.";
    case settings::UnitState::Stale:
        return "Activado, pero el servicio está desactualizado: guarda para actualizarlo.";
    case settings::UnitState::Current:
        break;
    }
    if (!running) {
        return std::string{"Activado, pero el servicio "} +
               (status.active == "failed" ? "falló" : "está detenido") +
               ": guarda para arrancarlo.";
    }
    return is_enabled ? ""
                      : "Activado, pero el servicio está deshabilitado: guarda para habilitarlo.";
}

std::string journal_command(const settings::AddonInfo& addon) {
    std::string_view unit = addon.unit;
    if (unit.ends_with(".service")) {
        unit.remove_suffix(std::string_view{".service"}.size()); // Más corto; systemd lo entiende.
    }
    return "journalctl --user -u " + std::string{unit} + " -f";
}

} // namespace chatbot::cli
