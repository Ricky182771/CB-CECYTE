#ifndef CHATBOT_ADDON_SETTINGS_ZONE_DATABASE_H
#define CHATBOT_ADDON_SETTINGS_ZONE_DATABASE_H

// La base de zonas de C++20, aparte: CMake compila zone_database_tzdb.cpp o
// zone_database_none.cpp según CHATBOT_TZDB (como la capa de plataforma del
// núcleo), así que no hay #if en la lógica.

#include "chatbot/addon_settings/time_zone.h"

#include <string>
#include <string_view>

namespace chatbot::addon_settings::detail {

/// true si esta compilación tiene la base de zonas.
[[nodiscard]] bool zone_database_available();

/// La fecha en la zona de IANA con ese nombre. Sin base de zonas, o con un
/// nombre que no existe, error Config (cause dice por qué).
[[nodiscard]] Result<DayFunction> named_zone_day(std::string_view name);

} // namespace chatbot::addon_settings::detail

#endif // CHATBOT_ADDON_SETTINGS_ZONE_DATABASE_H
