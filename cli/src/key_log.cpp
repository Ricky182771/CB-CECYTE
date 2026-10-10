#include "key_log.h"

#include "chatbot/platform.h"

#include <cstdio>
#include <ctime>

namespace chatbot::cli {

std::string hex_bytes(std::string_view bytes) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 3);
    for (const char c : bytes) {
        const auto byte = static_cast<unsigned char>(c);
        if (!out.empty()) {
            out += ' ';
        }
        out += kDigits[byte >> 4];
        out += kDigits[byte & 0x0F];
    }
    return out;
}

std::string_view paste_target_name(PasteTarget target) {
    switch (target) {
    case PasteTarget::SingleLine:
        return "single-line";
    case PasteTarget::MultiLine:
        return "multi-line";
    case PasteTarget::None:
        break;
    }
    return "none";
}

std::string key_log_time(std::chrono::system_clock::time_point now) {
    const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
                            now.time_since_epoch()) %
                        1000;
    const std::optional<std::tm> local = local_time(seconds);
    if (!local.has_value()) {
        return "??:??:??.???";
    }
    char buffer[16];
    (void)std::snprintf(buffer, sizeof buffer, "%02d:%02d:%02d.%03d", local->tm_hour,
                        local->tm_min, local->tm_sec, static_cast<int>(millis.count()));
    return buffer;
}

std::string format_key_event(std::string_view time, const KeyLogEvent& event,
                             const KeyLogState& state) {
    std::string line{time};
    switch (event.kind) {
    case KeyLogEvent::Kind::Special:
        line += " special [" + hex_bytes(event.input) + "]";
        break;
    case KeyLogEvent::Kind::Character:
        // Solo la longitud: el contenido puede ser la API key.
        line += " character len=" + std::to_string(event.input.size());
        break;
    case KeyLogEvent::Kind::Mouse:
        line += " mouse ";
        line += event.button;
        line += ' ';
        line += event.motion;
        line += " x=" + std::to_string(event.x) + " y=" + std::to_string(event.y);
        break;
    case KeyLogEvent::Kind::Custom:
        line += " custom";
        break;
    }
    line += " | settings=";
    line += state.settings_open ? "open" : "closed";
    line += " sidebar_focus=";
    line += state.sidebar_focused ? "yes" : "no";
    line += " target=";
    line += paste_target_name(state.paste_target);
    line += " bracketed=";
    line += state.bracketed_paste_open ? "open" : "closed";
    if (!state.focused_fields.empty()) {
        line += " focused:";
        for (const auto& [name, focused] : state.focused_fields) {
            line += ' ';
            line += name;
            line += '=';
            line += focused ? '1' : '0';
        }
    }
    return line;
}

std::string format_paste_shortcut(std::string_view time,
                                  std::optional<std::size_t> clipboard_bytes, bool delivered) {
    std::string line{time};
    line += " paste-shortcut clipboard=";
    line += clipboard_bytes.has_value() ? "len=" + std::to_string(*clipboard_bytes) : "nullopt";
    line += " delivered=";
    line += delivered ? "yes" : "no";
    return line;
}

KeyLog::KeyLog(const std::optional<std::string>& path) {
    if (!path.has_value() || path->empty()) {
        return;
    }
    try {
        file_.open(*path, std::ios::app | std::ios::binary);
    } catch (...) {
        // Depurar nunca debe romper la app.
    }
}

void KeyLog::write(std::string_view line) noexcept {
    if (!file_.is_open()) {
        return;
    }
    try {
        file_ << line << '\n';
        file_.flush();
    } catch (...) {
    }
}

} // namespace chatbot::cli
