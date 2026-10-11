#include "commands.h"

namespace chatbot::cli {

namespace {

constexpr std::string_view kNoProvider = "falta configurar el proveedor (F2)";
constexpr std::string_view kNoSearchKey = "falta la key de búsqueda (F2)";
constexpr std::string_view kNoCodeBlocks = "no hay bloques de código";
constexpr std::string_view kEmptyConversation = "la conversación está vacía";
constexpr std::string_view kBusy = "espera a que termine la respuesta";

/// Primero lo que no se arregla esperando; la respuesta en curso, al final.
CommandAvailability availability_of(std::string_view name, const CommandContext& context) {
    std::string_view note;
    if (name == kSearchCommand) {
        note = !context.configured       ? kNoProvider
               : !context.has_search_key ? kNoSearchKey
               : context.busy            ? kBusy
                                         : std::string_view{};
    } else if (name == kCopyCommand || name == kSaveCommand) {
        note = !context.has_code_blocks ? kNoCodeBlocks
               : context.busy           ? kBusy
                                        : std::string_view{};
    } else if (name == kExportCommand) {
        note = !context.has_turns ? kEmptyConversation
               : context.busy     ? kBusy
                                  : std::string_view{};
    }
    return CommandAvailability{note.empty(), note};
}

} // namespace

const CommandInfo* find_command(std::string_view name) {
    for (const CommandInfo& command : kCommands) {
        if (command.name == name) {
            return &command;
        }
    }
    return nullptr;
}

std::string_view command_name(ParsedCommand::Type type) {
    using Type = ParsedCommand::Type;
    switch (type) {
    case Type::Normal:
        return {};
    case Type::Search:
    case Type::SearchEmpty:
        return kSearchCommand;
    case Type::Copy:
    case Type::CopyUsage:
        return kCopyCommand;
    case Type::Save:
    case Type::SaveUsage:
        return kSaveCommand;
    case Type::Export:
    case Type::ExportUsage:
        return kExportCommand;
    }
    return {};
}

std::vector<CommandAvailability> command_availability(const CommandContext& context) {
    std::vector<CommandAvailability> out;
    out.reserve(kCommands.size());
    for (const CommandInfo& command : kCommands) {
        out.push_back(availability_of(command.name, context));
    }
    return out;
}

} // namespace chatbot::cli
