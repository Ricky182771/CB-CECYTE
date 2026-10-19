#include "zone_database.h"

#include "chatbot/error.h"

#include <chrono>
#include <cstdio>
#include <exception>
#include <optional>
#include <string>

namespace chatbot::bot::detail {

bool zone_database_available() {
    return true;
}

Result<DayFunction> named_zone_day(std::string_view name) {
    const std::chrono::time_zone* zone = nullptr;
    try {
        zone = std::chrono::locate_zone(name);
    } catch (const std::exception& e) {
        // libstdc++ da "cannot locate zone" o, sin tzdata, por qué no pudo
        // leer la base.
        return ChatError{ErrorKind::Config, 0, e.what(), std::nullopt};
    }
    if (zone == nullptr) {
        return ChatError{ErrorKind::Config, 0, "no existe", std::nullopt};
    }
    // El puntero vive lo que el proceso: la base de zonas no se libera.
    return DayFunction{[zone](std::chrono::system_clock::time_point when) {
        const std::chrono::zoned_time local{zone,
                                            std::chrono::floor<std::chrono::seconds>(when)};
        const std::chrono::year_month_day day{
            std::chrono::floor<std::chrono::days>(local.get_local_time())};
        char text[16] = {};
        std::snprintf(text, sizeof text, "%04d-%02u-%02u", static_cast<int>(day.year()),
                      static_cast<unsigned>(day.month()), static_cast<unsigned>(day.day()));
        return std::string{text};
    }};
}

} // namespace chatbot::bot::detail
