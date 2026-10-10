#include "command_parser.h"

#include <algorithm>
#include <cctype>
#include <utility>
#include <vector>

namespace chatbot::cli {

namespace {

bool is_space(char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; }

/// Separa los argumentos por espacios (cualquier cantidad).
std::vector<std::string_view> split_words(std::string_view text) {
    std::vector<std::string_view> words;
    std::size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && is_space(text[i])) {
            ++i;
        }
        const std::size_t begin = i;
        while (i < text.size() && !is_space(text[i])) {
            ++i;
        }
        if (i > begin) {
            words.push_back(text.substr(begin, i - begin));
        }
    }
    return words;
}

/// Número de bloque: solo dígitos, a lo más 9 (cabe en int).
std::optional<int> block_number(std::string_view word) {
    if (word.empty() || word.size() > 9 ||
        !std::all_of(word.begin(), word.end(),
                     [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; })) {
        return std::nullopt;
    }
    int value = 0;
    for (const char c : word) {
        value = value * 10 + (c - '0');
    }
    return value;
}

/// /copiar, /guardar y /exportar. input ya viene recortado. nullopt si no
/// es ninguno de ellos (por ejemplo "/copiarx").
std::optional<ParsedCommand> parse_block_command(std::string_view input) {
    const std::size_t end = static_cast<std::size_t>(
        std::find_if(input.begin(), input.end(), is_space) - input.begin());
    const std::string_view name = input.substr(0, end);
    const std::vector<std::string_view> args = split_words(input.substr(end));
    ParsedCommand command;
    if (name == "/copiar") {
        command.type = ParsedCommand::Type::Copy;
        if (args.size() == 1) {
            command.block = block_number(args[0]);
        }
        if (args.size() > 1 || (args.size() == 1 && !command.block.has_value())) {
            return ParsedCommand{ParsedCommand::Type::CopyUsage, "", std::nullopt, ""};
        }
        return command;
    }
    if (name == "/guardar") {
        command.type = ParsedCommand::Type::Save;
        if (!args.empty()) {
            command.block = block_number(args[0]);
        }
        if (args.size() == 2) {
            command.file_name = std::string{args[1]};
        }
        if (args.size() > 2 || (!args.empty() && !command.block.has_value())) {
            return ParsedCommand{ParsedCommand::Type::SaveUsage, "", std::nullopt, ""};
        }
        return command;
    }
    if (name == "/exportar") {
        command.type = args.empty() ? ParsedCommand::Type::Export
                                    : ParsedCommand::Type::ExportUsage;
        return command;
    }
    return std::nullopt;
}

} // namespace

ParsedCommand parse_command(std::string_view input) {
    // Recortar espacios al inicio y al final.
    while (!input.empty() && std::isspace(static_cast<unsigned char>(input.front()))) {
        input.remove_prefix(1);
    }
    while (!input.empty() && std::isspace(static_cast<unsigned char>(input.back()))) {
        input.remove_suffix(1);
    }

    if (std::optional<ParsedCommand> command = parse_block_command(input)) {
        return *std::move(command);
    }

    // Verificar si empieza con "/buscar".
    constexpr std::string_view kSearchCommand = "/buscar";
    if (input.size() < kSearchCommand.size() ||
        input.substr(0, kSearchCommand.size()) != kSearchCommand) {
        return {ParsedCommand::Type::Normal, std::string{input}, std::nullopt, ""};
    }

    // Si es exactamente "/buscar" sin más texto, es un comando vacío.
    if (input.size() == kSearchCommand.size()) {
        return {ParsedCommand::Type::SearchEmpty, "", std::nullopt, ""};
    }

    // Debe haber un espacio (o un salto de línea) después de "/buscar".
    if (!is_space(input[kSearchCommand.size()])) {
        // Es algo como "/buscarx", no es el comando.
        return {ParsedCommand::Type::Normal, std::string{input}, std::nullopt, ""};
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
        return {ParsedCommand::Type::SearchEmpty, "", std::nullopt, ""};
    }

    // Tavily recibe una sola línea: los saltos pasan a espacios.
    std::string text{query};
    std::replace(text.begin(), text.end(), '\n', ' ');
    return {ParsedCommand::Type::Search, std::move(text), std::nullopt, ""};
}

}  // namespace chatbot::cli
