#ifndef CHATBOT_ADDON_SETTINGS_TIME_ZONE_H
#define CHATBOT_ADDON_SETTINGS_TIME_ZONE_H

#include "chatbot/result.h"

#include <chrono>
#include <functional>
#include <string>
#include <string_view>

namespace chatbot::addon_settings {

/// La fecha "AAAA-MM-DD" de un instante en alguna zona horaria: con ella
/// cambia de día el tope diario y se fecha /buscar.
using DayFunction = std::function<std::string(std::chrono::system_clock::time_point)>;

/// Zona horaria predeterminada de los addons: la de la escuela.
inline constexpr std::string_view kDefaultTimeZone = "America/Mexico_City";
/// Su desfase fijo, para las compilaciones sin base de zonas: la Ciudad de
/// México no tiene horario de verano desde 2022, así que es lo mismo.
inline constexpr std::string_view kDefaultUtcOffset = "-06:00";

/// true si esta compilación tiene la base de zonas de C++20
/// (std::chrono::locate_zone). CMake lo detecta compilando y enlazando una
/// prueba (CHATBOT_TZDB): libstdc++ 13 o posterior sí; libc++ (Clang en
/// Termux, macOS) puede no tenerla.
[[nodiscard]] bool time_zone_database_available();

/// kDefaultTimeZone si hay base de zonas; si no, kDefaultUtcOffset.
[[nodiscard]] std::string_view default_time_zone();

/// Desfase fijo "+HH:MM" o "-HH:MM" (signo obligatorio, dos dígitos en cada
/// parte, de -12:00 a +14:00, minutos 00, 15, 30 o 45) en minutos. Si no,
/// error Config con el motivo.
[[nodiscard]] Result<std::chrono::minutes> parse_utc_offset(std::string_view text);

/// La fecha con un desfase fijo respecto de UTC (sin horario de verano).
[[nodiscard]] DayFunction day_at_offset(std::chrono::minutes offset);

/// La fecha en una zona: un nombre de la base de IANA (p. ej.
/// "America/Mexico_City", con std::chrono::locate_zone: no cambia la zona
/// de todo el proceso ni depende de TZ) o un desfase fijo como "-06:00"
/// (parse_utc_offset), que funciona en cualquier compilación. Un nombre sin
/// base de zonas, un nombre desconocido o sin tzdata en el sistema: error
/// Config que dice cómo arreglarlo. La función que devuelve es segura entre
/// hilos.
[[nodiscard]] Result<DayFunction> day_in_zone(std::string_view name);

} // namespace chatbot::addon_settings

#endif // CHATBOT_ADDON_SETTINGS_TIME_ZONE_H
