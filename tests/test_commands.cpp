#include <catch2/catch_test_macros.hpp>

#include "command_parser.h"
#include "commands.h"

#include <string>
#include <string_view>

using chatbot::cli::CommandAvailability;
using chatbot::cli::CommandContext;
using chatbot::cli::CommandInfo;
using chatbot::cli::kCommands;
using chatbot::cli::ParsedCommand;

namespace {

/// Disponibilidad del comando name (que debe estar en el registro).
CommandAvailability availability_of(std::string_view name, const CommandContext& context) {
    const std::vector<CommandAvailability> all = chatbot::cli::command_availability(context);
    REQUIRE(all.size() == kCommands.size());
    for (std::size_t i = 0; i < kCommands.size(); ++i) {
        if (kCommands[i].name == name) {
            return all[i];
        }
    }
    FAIL("No está en el registro: " << name);
    return {};
}

/// Todo disponible.
CommandContext ready() { return CommandContext{true, true, true, true, false}; }

} // namespace

TEST_CASE("Cada comando del registro lo reconoce parse_command", "[commands]") {
    for (const CommandInfo& command : kCommands) {
        INFO(command.name);
        CHECK(command.name.front() == '/');
        const ParsedCommand parsed = chatbot::cli::parse_command(command.name);
        CHECK(parsed.type != ParsedCommand::Type::Normal);
        CHECK(chatbot::cli::command_name(parsed.type) == command.name);
        // Con un argumento también es el mismo comando (bien o mal usado).
        const ParsedCommand with_argument =
            chatbot::cli::parse_command(std::string{command.name} + " 1");
        CHECK(chatbot::cli::command_name(with_argument.type) == command.name);
    }
}

TEST_CASE("Ningún tipo de ParsedCommand queda sin entrada en el registro", "[commands]") {
    using Type = ParsedCommand::Type;
    for (const Type type : {Type::Search, Type::SearchEmpty, Type::Copy, Type::CopyUsage,
                            Type::Save, Type::SaveUsage, Type::Export, Type::ExportUsage}) {
        INFO(static_cast<int>(type));
        CHECK(chatbot::cli::find_command(chatbot::cli::command_name(type)) != nullptr);
    }
    CHECK(chatbot::cli::command_name(Type::Normal).empty());
}

TEST_CASE("Los argumentos del registro coinciden con los avisos de uso", "[commands]") {
    const auto usage = [](std::string_view name) {
        const CommandInfo* command = chatbot::cli::find_command(name);
        REQUIRE(command != nullptr);
        return "Uso: " + std::string{command->name} + " " + std::string{command->arguments};
    };
    CHECK(usage("/buscar") == chatbot::cli::kSearchUsage);
    CHECK(usage("/copiar") == chatbot::cli::kCopyUsage);
    CHECK(usage("/guardar") == chatbot::cli::kSaveUsage);
    const CommandInfo* exportar = chatbot::cli::find_command("/exportar");
    REQUIRE(exportar != nullptr);
    CHECK(exportar->arguments.empty());
}

TEST_CASE("Registro: nombres en minúsculas, sin repetir y descripciones cortas", "[commands]") {
    for (std::size_t i = 0; i < kCommands.size(); ++i) {
        const CommandInfo& command = kCommands[i];
        INFO(command.name);
        for (const char c : command.name) {
            CHECK((c == '/' || (c >= 'a' && c <= 'z')));
        }
        CHECK(!command.description.empty());
        // Unas 50 columnas (las descripciones son de caracteres angostos;
        // cada acento ocupa 2 bytes).
        CHECK(command.description.size() <= 56);
        for (std::size_t j = i + 1; j < kCommands.size(); ++j) {
            CHECK(command.name != kCommands[j].name);
        }
    }
    CHECK(chatbot::cli::find_command("/buscar")->needs_argument);
    CHECK_FALSE(chatbot::cli::find_command("/copiar")->needs_argument);
    CHECK_FALSE(chatbot::cli::find_command("/guardar")->needs_argument);
    CHECK_FALSE(chatbot::cli::find_command("/exportar")->needs_argument);
    CHECK(chatbot::cli::find_command("/BUSCAR") == nullptr);
    CHECK(chatbot::cli::find_command("/bus") == nullptr);
}

TEST_CASE("Disponibilidad de los comandos", "[commands]") {
    SECTION("Con todo listo, todos disponibles y sin nota") {
        for (const CommandAvailability& state : chatbot::cli::command_availability(ready())) {
            CHECK(state.available);
            CHECK(state.note.empty());
        }
    }
    SECTION("/buscar sin proveedor o sin key") {
        CommandContext context = ready();
        context.has_search_key = false;
        CHECK_FALSE(availability_of("/buscar", context).available);
        CHECK(availability_of("/buscar", context).note == "falta la key de búsqueda (F2)");
        context.configured = false;
        CHECK(availability_of("/buscar", context).note == "falta configurar el proveedor (F2)");
        // Los demás no necesitan proveedor ni key.
        CHECK(availability_of("/copiar", context).available);
        CHECK(availability_of("/exportar", context).available);
    }
    SECTION("/copiar y /guardar sin bloques de código") {
        CommandContext context = ready();
        context.has_code_blocks = false;
        CHECK(availability_of("/copiar", context).note == "no hay bloques de código");
        CHECK(availability_of("/guardar", context).note == "no hay bloques de código");
        CHECK(availability_of("/exportar", context).available);
    }
    SECTION("/exportar con la conversación vacía") {
        CommandContext context = ready();
        context.has_turns = false;
        CHECK(availability_of("/exportar", context).note == "la conversación está vacía");
    }
    SECTION("Con una respuesta en curso, ninguno") {
        CommandContext context = ready();
        context.busy = true;
        for (const CommandAvailability& state : chatbot::cli::command_availability(context)) {
            CHECK_FALSE(state.available);
            CHECK(state.note == "espera a que termine la respuesta");
        }
    }
    SECTION("Lo que no se arregla esperando va antes que la respuesta en curso") {
        CommandContext context{};
        context.busy = true;
        CHECK(availability_of("/buscar", context).note == "falta configurar el proveedor (F2)");
        CHECK(availability_of("/copiar", context).note == "no hay bloques de código");
        CHECK(availability_of("/exportar", context).note == "la conversación está vacía");
    }
}
