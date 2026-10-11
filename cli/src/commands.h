#ifndef CHATBOT_CLI_COMMANDS_H
#define CHATBOT_CLI_COMMANDS_H

#include "command_parser.h"

#include <array>
#include <string_view>
#include <vector>

namespace chatbot::cli {

/// Nombres de los comandos: los usan el registro (kCommands) y parse_command.
inline constexpr std::string_view kSearchCommand = "/buscar";
inline constexpr std::string_view kCopyCommand = "/copiar";
inline constexpr std::string_view kSaveCommand = "/guardar";
inline constexpr std::string_view kExportCommand = "/exportar";

/// Un comando de la caja, como lo muestra el menú de comandos.
struct CommandInfo {
    std::string_view name;        ///< "/buscar" (siempre en minúsculas).
    std::string_view arguments;   ///< "<consulta>", "[número de bloque]" o "".
    std::string_view description; ///< Corta, para unas 50 columnas.
    bool needs_argument;          ///< Sin argumento, el comando no sirve (/buscar).
};

/// Registro de los comandos, en el orden en que los muestra el menú. Es la
/// única lista: los argumentos coinciden con los avisos de uso
/// (kSearchUsage, kCopyUsage, kSaveUsage), y las pruebas lo revisan.
inline constexpr std::array<CommandInfo, 4> kCommands = {{
    {kSearchCommand, "<consulta>", "Busca en la web y responde con fuentes citadas", true},
    {kCopyCommand, "[número de bloque]", "Copia un bloque de código; sin número, el último",
     false},
    {kSaveCommand, "[número de bloque [nombre]]", "Guarda un bloque de código en un archivo",
     false},
    {kExportCommand, "", "Exporta la conversación a Markdown", false},
}};

/// El comando del registro con ese nombre exacto, o nullptr.
[[nodiscard]] const CommandInfo* find_command(std::string_view name);

/// Nombre del comando que produce un tipo de parse_command (vacío para
/// Normal). Con un tipo nuevo sin caso, el switch no compila (-Wswitch).
[[nodiscard]] std::string_view command_name(ParsedCommand::Type type);

/// Lo que hace falta saber para decir qué comandos se pueden usar ahora.
struct CommandContext {
    bool configured = false;      ///< Hay proveedor y modelo (hay runner).
    bool has_search_key = false;  ///< Hay key de búsqueda web.
    bool has_code_blocks = false; ///< La conversación tiene bloques de código.
    bool has_turns = false;       ///< Hay al menos un par pregunta/respuesta.
    bool busy = false;            ///< Hay una respuesta en curso.
};

/// Si un comando se puede usar ahora y, si no, una nota corta del porqué.
struct CommandAvailability {
    bool available = true;
    std::string_view note; ///< Vacía si available.
};

/// Disponibilidad de cada comando de kCommands, en el mismo orden. Un
/// comando no disponible se puede elegir igual: al enviarlo aparece el
/// aviso o el error de uso de siempre.
[[nodiscard]] std::vector<CommandAvailability> command_availability(
    const CommandContext& context);

} // namespace chatbot::cli

#endif // CHATBOT_CLI_COMMANDS_H
