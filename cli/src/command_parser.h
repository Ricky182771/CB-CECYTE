#ifndef CHATBOT_CLI_COMMAND_PARSER_H
#define CHATBOT_CLI_COMMAND_PARSER_H

#include <optional>
#include <string>
#include <string_view>

namespace chatbot::cli {

/// Resultado del parseo de un comando.
struct ParsedCommand {
    enum class Type {
        Normal,    ///< Texto normal (no es un comando).
        Search,    ///< Comando /buscar con una consulta.
        SearchEmpty ///< Comando /buscar sin consulta (error de uso).
    };

    Type type = Type::Normal;
    std::string text; ///< Para Normal: el texto completo. Para Search: la consulta.
};

/// Parsea el texto de entrada para detectar el comando /buscar.
/// - `/buscar <consulta>`: devuelve Search con la consulta (sin espacios en los extremos).
/// - `/buscar` sin consulta: devuelve SearchEmpty.
/// - Cualquier otro texto (incluido `/buscarx` u otros comandos): devuelve Normal.
ParsedCommand parse_command(std::string_view input);

}  // namespace chatbot::cli

#endif  // CHATBOT_CLI_COMMAND_PARSER_H
