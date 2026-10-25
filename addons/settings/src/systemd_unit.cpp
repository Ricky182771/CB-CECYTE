#include "chatbot/addon_settings/systemd_unit.h"

#include <cstdio>

namespace chatbot::addon_settings {
namespace {

/// Comillas dobles de systemd. dollar: también $ → $$ (ExecStart= expande
/// variables; Environment= no).
std::string quote(std::string_view text, bool dollar) {
    std::string out = "\"";
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        if (c == '\\' || c == '"') {
            out += '\\';
            out += c;
        } else if (c == '%') {
            out += "%%";
        } else if (c == '$' && dollar) {
            out += "$$";
        } else if (byte < 0x20 || byte == 0x7F) {
            char escaped[8] = {};
            std::snprintf(escaped, sizeof escaped, "\\x%02x", byte);
            out += escaped;
        } else {
            out += c;
        }
    }
    return out + "\"";
}

} // namespace

std::string quote_exec_argument(std::string_view argument) {
    return quote(argument, true);
}

std::string quote_environment(std::string_view name, std::string_view value) {
    std::string assignment{name};
    assignment += '=';
    assignment += value;
    return quote(assignment, false);
}

std::string unit_text(const UnitParams& params) {
    std::string text{kUnitMarker};
    text += "\n[Unit]\n";
    text += "Description=" + params.description + "\n";
    text += "\n[Service]\n";
    text += "Type=exec\n";
    text += "ExecStart=" + quote_exec_argument(params.chatbot_path) + " " +
            quote_exec_argument(params.addon) + "\n";
    text += "ExecReload=kill -HUP $MAINPID\n";
    text += "Restart=on-failure\n";
    text += "RestartSec=10\n";
    text += "RestartPreventExitStatus=65 69 73 78\n";
    const auto environment = [&text](std::string_view name,
                                     const std::optional<std::string>& value) {
        if (value.has_value() && !value->empty() && value->front() == '/') {
            text += "Environment=" + quote_environment(name, *value) + "\n";
        }
    };
    environment("XDG_CONFIG_HOME", params.xdg_config_home);
    environment("XDG_DATA_HOME", params.xdg_data_home);
    text += "\n[Install]\n";
    text += "WantedBy=default.target\n";
    return text;
}

UnitState classify_unit(const std::optional<std::string>& existing, const UnitParams& params) {
    if (!existing.has_value()) {
        return UnitState::Missing;
    }
    const std::string_view text{*existing};
    if (text.substr(0, kUnitMarker.size()) != kUnitMarker ||
        (text.size() > kUnitMarker.size() && text[kUnitMarker.size()] != '\n')) {
        return UnitState::Foreign;
    }
    return *existing == unit_text(params) ? UnitState::Current : UnitState::Stale;
}

} // namespace chatbot::addon_settings
