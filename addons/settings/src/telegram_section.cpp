#include "chatbot/addon_settings/telegram_section.h"

#include "chatbot/config.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <exception>
#include <limits>
#include <utility>

namespace chatbot::addon_settings {
namespace {

using ordered = nlohmann::ordered_json;

constexpr std::string_view kTelegramKey = "telegram";

/// Un entero que cabe en std::int64_t (los positivos llegan sin signo).
std::optional<std::int64_t> as_integer(const ordered& value) {
    if (value.is_number_unsigned()) {
        const auto number = value.get<std::uint64_t>();
        if (number > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            return std::nullopt;
        }
        return static_cast<std::int64_t>(number);
    }
    if (value.is_number_integer()) {
        return value.get<std::int64_t>();
    }
    return std::nullopt;
}

/// Lee una llave conocida en section; false si es de otro tipo.
bool read_key(const std::string& key, const ordered& value, TelegramSectionRead& read) {
    TelegramSection& out = read.values;
    const auto text = [&value](std::string& target) {
        if (!value.is_string()) {
            return false;
        }
        target = value.get<std::string>();
        return true;
    };
    const auto flag = [&value](bool& target) {
        if (!value.is_boolean()) {
            return false;
        }
        target = value.get<bool>();
        return true;
    };
    const auto number = [&value](std::int64_t& target) {
        const std::optional<std::int64_t> parsed = as_integer(value);
        if (parsed.has_value()) {
            target = *parsed;
        }
        return parsed.has_value();
    };
    if (key == "version") {
        read.version = as_integer(value);
        return read.version.has_value();
    }
    if (key == "enabled") {
        return flag(out.enabled);
    }
    if (key == "model") {
        return text(out.model);
    }
    if (key == "allowed_users") {
        if (!value.is_array()) {
            return false;
        }
        std::vector<std::int64_t> users;
        for (const ordered& item : value) {
            const std::optional<std::int64_t> user = as_integer(item);
            if (!user.has_value()) {
                return false;
            }
            users.push_back(*user);
        }
        out.allowed_users = std::move(users);
        return true;
    }
    if (key == "invite_code") {
        return text(out.invite_code);
    }
    if (key == "daily_message_limit") {
        return number(out.daily_message_limit);
    }
    if (key == "max_concurrent_responses") {
        return number(out.max_concurrent_responses);
    }
    if (key == "session_idle_minutes") {
        return number(out.session_idle_minutes);
    }
    if (key == "stale_message_minutes") {
        return number(out.stale_message_minutes);
    }
    if (key == "draft_interval_ms") {
        return number(out.draft_interval_ms);
    }
    if (key == "time_zone") {
        return text(out.time_zone);
    }
    if (key == "log_error_details") {
        return flag(out.log_error_details);
    }
    return false; // Llave desconocida.
}

std::string join(const std::vector<std::string>& keys) {
    std::string text;
    for (const std::string& key : keys) {
        text += (text.empty() ? "\"" : ", \"") + key + "\"";
    }
    return text;
}

} // namespace

std::optional<std::string> TelegramSectionRead::problem() const {
    if (!present) {
        return std::nullopt;
    }
    if (newer()) {
        return "La sección \"addons.telegram\" de config.json es de la versión " +
               std::to_string(*version) + " y este chatbot-telegram entiende la " +
               std::to_string(kTelegramConfigVersion) + ": actualiza chatbot-telegram.";
    }
    if (!discarded.empty()) {
        return "La sección \"addons.telegram\" de config.json tiene llaves que no se entienden "
               "(desconocidas, de otro tipo o sin \"version\"): " +
               join(discarded) + ". Corrígela en Configuración → Addons.";
    }
    return std::nullopt;
}

Result<TelegramSectionRead> read_telegram_section(const std::string& config_path) {
    Result<std::optional<std::string>> addons =
        load_config_section(config_path, kAddonsKey);
    if (addons.is_error()) {
        return addons.error();
    }
    TelegramSectionRead read;
    if (!addons.value().has_value()) {
        return read;
    }
    try {
        const ordered all = ordered::parse(*addons.value());
        if (!all.is_object()) {
            read.present = true;
            read.discarded.emplace_back(kAddonsKey);
            return read;
        }
        const auto section = all.find(std::string{kTelegramKey});
        if (section == all.end()) {
            return read;
        }
        read.present = true;
        if (!section->is_object()) {
            read.discarded.emplace_back(kTelegramKey);
            return read;
        }
        for (auto it = section->begin(); it != section->end(); ++it) {
            if (!read_key(it.key(), it.value(), read)) {
                read.discarded.push_back(it.key());
            }
        }
        // Sin "version", o una que no es la de este esquema ni una mayor (0,
        // negativa): no se sabe qué significan las llaves.
        const bool version_listed =
            std::find(read.discarded.begin(), read.discarded.end(), "version") !=
            read.discarded.end();
        if (!version_listed && (!read.version.has_value() || *read.version < 1)) {
            read.discarded.emplace_back("version");
        }
    } catch (const std::exception&) {
        return ChatError{ErrorKind::Config, 0,
                         "Archivo de configuración inválido (" + config_path + ").",
                         std::nullopt};
    }
    return read;
}

std::optional<ChatError> write_telegram_section(const std::string& config_path,
                                                const TelegramSection& section) {
    Result<std::optional<std::string>> addons =
        load_config_section(config_path, kAddonsKey);
    if (addons.is_error()) {
        return addons.error();
    }
    try {
        ordered all = ordered::object();
        if (addons.value().has_value()) {
            all = ordered::parse(*addons.value());
            if (!all.is_object()) {
                all = ordered::object(); // No se entendía: se reemplaza.
            }
        }
        ordered out = ordered::object();
        out["version"] = kTelegramConfigVersion;
        out["enabled"] = section.enabled;
        if (!section.model.empty()) {
            out["model"] = section.model;
        }
        out["allowed_users"] = section.allowed_users;
        out["invite_code"] = section.invite_code;
        out["daily_message_limit"] = section.daily_message_limit;
        out["max_concurrent_responses"] = section.max_concurrent_responses;
        out["session_idle_minutes"] = section.session_idle_minutes;
        out["stale_message_minutes"] = section.stale_message_minutes;
        out["draft_interval_ms"] = section.draft_interval_ms;
        if (!section.time_zone.empty()) {
            out["time_zone"] = section.time_zone;
        }
        out["log_error_details"] = section.log_error_details;
        all[std::string{kTelegramKey}] = std::move(out);
        return save_config_section(config_path, kAddonsKey,
                                   all.dump(-1, ' ', false, ordered::error_handler_t::replace));
    } catch (const std::exception&) {
        return ChatError{ErrorKind::Config, 0,
                         "No se guardó la configuración del addon de Telegram.", std::nullopt};
    }
}

} // namespace chatbot::addon_settings
