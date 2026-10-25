#include "chatbot/addon_settings/time_zone.h"

#include "zone_database.h"

#include "chatbot/error.h"

#include <cstdio>
#include <optional>
#include <string>

namespace chatbot::addon_settings {
namespace {

/// Desfases de UTC que existen: de -12:00 a +14:00.
constexpr std::chrono::minutes kMinOffset{-12 * 60};
constexpr std::chrono::minutes kMaxOffset{14 * 60};

ChatError zone_error(std::string_view name, const std::string& cause) {
    // Sin base de zonas solo sirve el desfase: no se sugiere un nombre.
    const std::string hint = detail::zone_database_available()
                                 ? "Usa un nombre como " + std::string{kDefaultTimeZone} +
                                       " o un desfase fijo como " + std::string{kDefaultUtcOffset}
                                 : "Usa un desfase fijo como " + std::string{kDefaultUtcOffset};
    return ChatError{ErrorKind::Config, 0,
                     "Zona horaria \"" + std::string{name} + "\" no válida: " + cause + ". " +
                         hint + " (sin horario de verano).",
                     std::nullopt};
}

bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

} // namespace

bool time_zone_database_available() {
    return detail::zone_database_available();
}

std::string_view default_time_zone() {
    return time_zone_database_available() ? kDefaultTimeZone : kDefaultUtcOffset;
}

Result<std::chrono::minutes> parse_utc_offset(std::string_view text) {
    const auto invalid = [text](const char* why) {
        return ChatError{ErrorKind::Config, 0,
                         "Desfase \"" + std::string{text} + "\" no válido: " + why + ".",
                         std::nullopt};
    };
    if (text.size() != 6 || (text[0] != '+' && text[0] != '-') || !is_digit(text[1]) ||
        !is_digit(text[2]) || text[3] != ':' || !is_digit(text[4]) || !is_digit(text[5])) {
        return invalid("debe ser +HH:MM o -HH:MM, por ejemplo -06:00");
    }
    const int hours = (text[1] - '0') * 10 + (text[2] - '0');
    const int minutes = (text[4] - '0') * 10 + (text[5] - '0');
    if (minutes % 15 != 0) {
        return invalid("los minutos deben ser 00, 15, 30 o 45");
    }
    const std::chrono::minutes offset{(text[0] == '-' ? -1 : 1) * (hours * 60 + minutes)};
    if (offset < kMinOffset || offset > kMaxOffset) {
        return invalid("debe estar entre -12:00 y +14:00");
    }
    return offset;
}

DayFunction day_at_offset(std::chrono::minutes offset) {
    return [offset](std::chrono::system_clock::time_point when) {
        const auto local = std::chrono::floor<std::chrono::days>(
            std::chrono::floor<std::chrono::minutes>(when) + offset);
        const std::chrono::year_month_day day{local};
        char text[16] = {};
        std::snprintf(text, sizeof text, "%04d-%02u-%02u", static_cast<int>(day.year()),
                      static_cast<unsigned>(day.month()), static_cast<unsigned>(day.day()));
        return std::string{text};
    };
}

Result<DayFunction> day_in_zone(std::string_view name) {
    if (name.empty()) {
        return zone_error(name, "está vacía");
    }
    if (name.front() == '+' || name.front() == '-') {
        const Result<std::chrono::minutes> offset = parse_utc_offset(name);
        if (offset.is_error()) {
            return ChatError{ErrorKind::Config, 0, "Zona horaria: " + offset.error().message,
                             std::nullopt};
        }
        return day_at_offset(offset.value());
    }
    Result<DayFunction> named = detail::named_zone_day(name);
    if (named.is_error()) {
        return zone_error(name, named.error().message);
    }
    return named;
}

} // namespace chatbot::addon_settings
