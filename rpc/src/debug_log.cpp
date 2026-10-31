#include "debug_log.h"

#include "protocol.h"

#include <array>

namespace chatbot::rpc {

namespace {

// invite_code: el código de invitación del bot (form.fields de addons.telegram.*).
constexpr std::array<std::string_view, 5> kSecretKeys{"api_key", "token", "model_key",
                                                      "search_key", "invite_code"};

bool is_secret(const std::string& key) {
    for (const std::string_view secret : kSecretKeys) {
        if (key == secret) {
            return true;
        }
    }
    return false;
}

} // namespace

Json redact_secrets(Json message) {
    if (message.is_object()) {
        for (auto& [key, value] : message.items()) {
            value = is_secret(key) ? Json(std::string{kRedacted})
                                   : redact_secrets(std::move(value));
        }
    } else if (message.is_array()) {
        for (Json& value : message) {
            value = redact_secrets(std::move(value));
        }
    }
    return message;
}

DebugLog::DebugLog(const std::string& path) {
    if (!path.empty()) {
        file_.open(path, std::ios::app | std::ios::binary);
    }
}

void DebugLog::incoming(std::string_view line) {
    if (!file_.is_open()) {
        return;
    }
    const Json parsed = Json::parse(line.begin(), line.end(), nullptr, false);
    if (parsed.is_discarded()) {
        write("→ ", "(no es JSON, " + std::to_string(line.size()) + " bytes)\n");
    } else {
        write("→ ", to_line(redact_secrets(parsed)));
    }
}

void DebugLog::outgoing(const Json& message) {
    if (file_.is_open()) {
        write("← ", to_line(redact_secrets(message)));
    }
}

void DebugLog::note(std::string_view text) {
    if (file_.is_open()) {
        write("→ ", "(" + std::string{text} + ")\n");
    }
}

void DebugLog::write(std::string_view prefix, std::string_view text) {
    file_ << prefix << text << std::flush;
}

} // namespace chatbot::rpc
