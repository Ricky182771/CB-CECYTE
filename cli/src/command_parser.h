#ifndef CHATBOT_CLI_COMMAND_PARSER_H
#define CHATBOT_CLI_COMMAND_PARSER_H

#include <optional>
#include <string>
#include <string_view>

namespace chatbot::cli {

/// Resultado del parseo de un comando.
struct ParsedCommand {
    enum class Type {
        Normal,      ///< Texto normal (no es un comando).
        Search,      ///< Comando /buscar con una consulta.
        SearchEmpty, ///< Comando /buscar sin consulta (error de uso).
        Copy,        ///< /copiar o /copiar <n>.
        CopyUsage,   ///< /copiar con argumentos que no son un número (error de uso).
        Save,        ///< /guardar, /guardar <n> o /guardar <n> <nombre>.
        SaveUsage,   ///< /guardar con argumentos de más o mal formados (error de uso).
        Export,      ///< /exportar.
        ExportUsage, ///< /exportar con argumentos (error de uso).
    };

    Type type = Type::Normal;
    std::string text; ///< Para Normal: el texto completo. Para Search: la consulta.
    /// Copy y Save: número del bloque que escribió el usuario (puede estar
    /// fuera de rango; eso lo revisa quien lo usa), o nullopt para el último.
    std::optional<int> block;
    /// Save: nombre que escribió el usuario, o vacío para el nombre por
    /// defecto. Sin validar (ver validate_file_name en downloads.h).
    std::string file_name;
};

/// Parsea el texto de entrada para detectar un comando.
/// - `/buscar <consulta>`: devuelve Search con la consulta (sin espacios en los extremos).
/// - `/buscar` sin consulta: devuelve SearchEmpty.
/// - `/copiar [n]`, `/guardar [n [nombre]]` y `/exportar`: Copy, Save y
///   Export; con otros argumentos, CopyUsage, SaveUsage y ExportUsage. n son
///   solo dígitos (a lo más 9); los argumentos se separan con espacios.
/// - Cualquier otro texto (incluido `/buscarx`, `/copiarx` u otros comandos): devuelve Normal.
ParsedCommand parse_command(std::string_view input);

}  // namespace chatbot::cli

#endif  // CHATBOT_CLI_COMMAND_PARSER_H
