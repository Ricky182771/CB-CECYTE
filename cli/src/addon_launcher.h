#ifndef CHATBOT_CLI_ADDON_LAUNCHER_H
#define CHATBOT_CLI_ADDON_LAUNCHER_H

#include "chatbot/addon_settings/registry.h"
#include "chatbot/platform.h"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/// La línea de comandos de chatbot: sin argumentos, la interfaz; --help; y
/// chatbot <addon> [argumentos], que busca el programa del addon (como git
/// con sus subcomandos) y se reemplaza por él con exec. Sin FTXUI.
namespace chatbot::cli {

/// Códigos de salida de chatbot (los de sysexits.h donde aplica).
inline constexpr int kExitUsage = 2;        ///< Argumentos que no se entienden.
inline constexpr int kExitUnavailable = 69; ///< EX_UNAVAILABLE: el addon no está instalado.

struct CommandLine {
    enum class Kind {
        Interface, ///< Sin argumentos: la interfaz de siempre.
        Help,      ///< --help o -h.
        Addon,     ///< chatbot <addon> [argumentos].
        Usage,     ///< Otra cosa: problem dice qué.
    };
    Kind kind = Kind::Interface;
    const addon_settings::AddonInfo* addon = nullptr; ///< Solo con Addon.
    std::vector<std::string> arguments;               ///< Los del addon, tal cual.
    std::string problem;                              ///< Solo con Usage.
};

/// Interpreta los argumentos (sin argv[0]). Solo los nombres del registro
/// (known_addons) son addons.
[[nodiscard]] CommandLine parse_command_line(const std::vector<std::string>& args);

using ExecutableCheck = std::function<bool(const std::string& path)>;
using PathSearch = std::function<std::optional<std::string>(std::string_view program)>;

/// Dónde está el programa del addon: primero junto a chatbot
/// (executable_dir) y, fuera del modo portable, en PATH. En modo portable
/// solo junto a chatbot: así nunca se arranca un bot de otra instalación
/// con estos datos.
[[nodiscard]] std::optional<std::string> find_addon_executable(
    const addon_settings::AddonInfo& addon, const std::string& executable_dir, bool portable,
    const ExecutableCheck& is_executable, const PathSearch& search_path);

/// find_addon_executable con el sistema real (executable_dir, el modo
/// portable de este proceso y PATH). En Windows, siempre nullopt.
[[nodiscard]] std::optional<std::string> locate_addon(const addon_settings::AddonInfo& addon);

/// "No encontré chatbot-telegram junto a chatbot ni en el PATH." (en modo
/// portable, sin "ni en el PATH").
[[nodiscard]] std::string addon_not_found_message(const addon_settings::AddonInfo& addon,
                                                  bool portable);

using AddonLocator =
    std::function<std::optional<std::string>(const addon_settings::AddonInfo& addon)>;

/// El texto de --help, con cada addon del registro y si está instalado
/// (locate). En Windows no busca: dice que los addons no están disponibles.
[[nodiscard]] std::string usage_text(Os os, bool portable, const AddonLocator& locate);

/// Atiende la línea de comandos: nullopt si hay que abrir la interfaz; si
/// no, el código de salida (la ayuda, un error de uso, un addon que no está).
/// Con un addon que sí está, no regresa: el proceso se reemplaza por él.
[[nodiscard]] std::optional<int> run_command_line(const std::vector<std::string>& args);

} // namespace chatbot::cli

#endif // CHATBOT_CLI_ADDON_LAUNCHER_H
