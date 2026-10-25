#ifndef CHATBOT_ADDON_SETTINGS_REGISTRY_H
#define CHATBOT_ADDON_SETTINGS_REGISTRY_H

#include <span>
#include <string_view>

namespace chatbot::addon_settings {

/// Lo que dicen chatbot <addon>, F2 → Addons y las funciones de los addons
/// en Windows.
inline constexpr std::string_view kAddonsUnavailableOnWindows =
    "Los addons todavía no están disponibles en Windows.";

/// Un addon conocido: chatbot <name> lo arranca y F2 → Addons lo configura.
/// Nada más se acepta como addon.
struct AddonInfo {
    std::string_view name;       ///< "telegram": el argumento de chatbot y la llave en "addons".
    std::string_view title;      ///< "Telegram": el título de su sección en F2.
    std::string_view executable; ///< "chatbot-telegram": el programa que se busca.
    int config_version = 0;      ///< La versión de su sección que entiende esta app.
    std::string_view unit;       ///< "chatbot-telegram.service": su servicio de systemd.
    std::string_view description; ///< Description= de su unidad de systemd.
};

/// Todos los addons conocidos, en el orden en que se muestran.
[[nodiscard]] std::span<const AddonInfo> known_addons();

/// El addon con ese nombre (exacto), o nullptr.
[[nodiscard]] const AddonInfo* find_addon(std::string_view name);

} // namespace chatbot::addon_settings

#endif // CHATBOT_ADDON_SETTINGS_REGISTRY_H
