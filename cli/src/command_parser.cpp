#include "command_parser.h"

#include <algorithm>
#include <cctype>

namespace chatbot::cli {

ParsedCommand parse_command(std::string_view input) {
    // Recortar espacios al inicio y al final.
    while (!input.empty() && std::isspace(static_cast<unsigned char>(input.front()))) {
        input.remove_prefix(1);
    }
    while (!input.empty() && std::isspace(static_cast<unsigned char>(input.back()))) {
        input.remove_suffix(1);
    }

    // Verificar si empieza con "/buscar".
    constexpr std::string_view kSearchCommand = "/buscar";
    if (input.size() < kSearchCommand.size() ||
        input.substr(0, kSearchCommand.size()) != kSearchCommand) {
        return {ParsedCommand::Type::Normal, std::string{input}};
    }

    // Si es exactamente "/buscar" sin más texto, es un comando vacío.
    if (input.size() == kSearchCommand.size()) {
        return {ParsedCommand::Type::SearchEmpty, ""};
    }

    // Debe haber un espacio después de "/buscar".
    if (input[kSearchCommand.size()] != ' ') {
        // Es algo como "/buscarx", no es el comando.
        return {ParsedCommand::Type::Normal, std::string{input}};
    }

    // Extraer la consulta (todo después de "/buscar ").
    std::string_view query = input.substr(kSearchCommand.size() + 1);

    // Recortar espacios de la consulta.
    while (!query.empty() && std::isspace(static_cast<unsigned char>(query.front()))) {
        query.remove_prefix(1);
    }
    while (!query.empty() && std::isspace(static_cast<unsigned char>(query.back()))) {
        query.remove_suffix(1);
    }

    // Si la consulta quedó vacía después de recortar, es un comando vacío.
    if (query.empty()) {
        return {ParsedCommand::Type::SearchEmpty, ""};
    }

    return {ParsedCommand::Type::Search, std::string{query}};
}

}  // namespace chatbot::cli
