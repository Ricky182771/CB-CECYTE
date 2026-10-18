#include "chatbot/bot/commands.h"

#include <catch2/catch_test_macros.hpp>

namespace {

using chatbot::bot::BotCommand;
using chatbot::bot::parse_bot_command;
using Type = chatbot::bot::BotCommand::Type;

} // namespace

TEST_CASE("comandos del bot: texto normal recortado", "[bot][comandos]") {
    const BotCommand command = parse_bot_command("  ¿Qué es un átomo?\n");
    CHECK(command.type == Type::Normal);
    CHECK(command.argument == "¿Qué es un átomo?");
    CHECK(parse_bot_command("   ").argument.empty());
}

TEST_CASE("comandos del bot: /start con y sin código", "[bot][comandos]") {
    CHECK(parse_bot_command("/start").type == Type::Start);
    CHECK(parse_bot_command("/start").argument.empty());
    const BotCommand with_code = parse_bot_command("/start  CODIGO_123 algo más");
    CHECK(with_code.type == Type::Start);
    CHECK(with_code.argument == "CODIGO_123");
    // En los grupos de Telegram el comando trae el nombre del bot.
    const BotCommand group = parse_bot_command("/start@MiBot CODIGO_123");
    CHECK(group.type == Type::Start);
    CHECK(group.argument == "CODIGO_123");
}

TEST_CASE("comandos del bot: /ayuda y /nuevo", "[bot][comandos]") {
    CHECK(parse_bot_command("/ayuda").type == Type::Help);
    CHECK(parse_bot_command("/nuevo por favor").type == Type::New);
    CHECK(parse_bot_command("/nuevo@MiBot").type == Type::New);
}

TEST_CASE("comandos del bot: /buscar", "[bot][comandos]") {
    const BotCommand search = parse_bot_command("/buscar  clima en\nPuebla  ");
    CHECK(search.type == Type::Search);
    CHECK(search.argument == "clima en Puebla");
    CHECK(parse_bot_command("/buscar").type == Type::SearchEmpty);
    CHECK(parse_bot_command("/buscar   ").type == Type::SearchEmpty);
}

TEST_CASE("comandos del bot: un comando desconocido es texto normal", "[bot][comandos]") {
    for (const char* text : {"/buscarx algo", "/START", "/help", "/copiar 2", "/"}) {
        INFO(text);
        const BotCommand command = parse_bot_command(text);
        CHECK(command.type == Type::Normal);
        CHECK(command.argument == text);
    }
}
