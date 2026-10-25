#ifndef CHATBOT_ADDON_SETTINGS_PATHS_H
#define CHATBOT_ADDON_SETTINGS_PATHS_H

#include <optional>
#include <string>
#include <string_view>

/// Dónde guarda sus datos un addon y dónde va su servicio de systemd. Las
/// funciones resolve_* son puras (reciben el entorno) para probar las reglas
/// sin tocar el del proceso; las default_* las llaman con el entorno real.
/// Solo reglas de Linux: los addons no están disponibles en Windows.
namespace chatbot::addon_settings {

/// Carpeta de datos del addon name (la del bot de Telegram guarda
/// invitados.json); no es configurable:
/// - en modo portable (portable_dir, de chatbot::portable_data_dir),
///   <portable_dir>/<name>, aunque estén XDG_DATA_HOME o HOME;
/// - si no, $XDG_DATA_HOME/chatbot/<name>, solo si es ruta absoluta (la
///   especificación XDG manda ignorar las relativas), como las
///   conversaciones de la app;
/// - si no, $HOME/.local/share/chatbot/<name>;
/// - nullopt si no hay nada.
[[nodiscard]] std::optional<std::string> resolve_addon_data_dir(
    std::string_view name, const std::optional<std::string>& portable_dir,
    const std::optional<std::string>& xdg_data_home, const std::optional<std::string>& home);

/// resolve_addon_data_dir con process_portable_data_dir() y el entorno real.
[[nodiscard]] std::optional<std::string> default_addon_data_dir(std::string_view name);

/// Ruta de invitados.json en la carpeta de datos del bot.
[[nodiscard]] std::string invited_file_path(std::string_view data_dir);

/// Ruta de la unidad de systemd de usuario unit: siempre
/// $HOME/.config/systemd/user/<unit>; nullopt sin HOME. Nunca con
/// XDG_CONFIG_HOME: systemd --user busca las unidades con su propio entorno,
/// que casi nunca lo tiene, y una unidad en otra carpeta no la encontraría
/// (la línea Environment=XDG_CONFIG_HOME de la unidad sí lo pasa al bot).
[[nodiscard]] std::optional<std::string> resolve_unit_path(
    std::string_view unit, const std::optional<std::string>& home);

/// resolve_unit_path con el entorno real.
[[nodiscard]] std::optional<std::string> default_unit_path(std::string_view unit);

} // namespace chatbot::addon_settings

#endif // CHATBOT_ADDON_SETTINGS_PATHS_H
