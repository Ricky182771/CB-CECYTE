#ifndef CHATBOT_ADDON_SETTINGS_RANDOM_BYTES_H
#define CHATBOT_ADDON_SETTINGS_RANDOM_BYTES_H

// Bytes al azar del sistema, aparte: CMake compila random_bytes_posix.cpp
// (getrandom) o random_bytes_windows.cpp (no disponible), sin #if en la
// lógica.

#include <optional>
#include <span>
#include <string>

namespace chatbot::addon_settings::detail {

/// Llena out con bytes al azar del sistema. El error en español, o nullopt.
[[nodiscard]] std::optional<std::string> random_bytes(std::span<unsigned char> out);

} // namespace chatbot::addon_settings::detail

#endif // CHATBOT_ADDON_SETTINGS_RANDOM_BYTES_H
