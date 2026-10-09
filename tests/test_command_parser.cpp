#include <catch2/catch_test_macros.hpp>

#include "command_parser.h"

using namespace chatbot::cli;

TEST_CASE("parse_command reconoce /buscar con consulta", "[command_parser]") {
    const auto result = parse_command("/buscar quién ganó el mundial");
    REQUIRE(result.type == ParsedCommand::Type::Search);
    REQUIRE(result.text == "quién ganó el mundial");
}

TEST_CASE("parse_command recorta espacios de la consulta", "[command_parser]") {
    const auto result = parse_command("/buscar   consulta con espacios   ");
    REQUIRE(result.type == ParsedCommand::Type::Search);
    REQUIRE(result.text == "consulta con espacios");
}

TEST_CASE("parse_command detecta /buscar sin consulta", "[command_parser]") {
    SECTION("Sin espacio después") {
        const auto result = parse_command("/buscar");
        REQUIRE(result.type == ParsedCommand::Type::SearchEmpty);
        REQUIRE(result.text.empty());
    }

    SECTION("Solo espacios después") {
        const auto result = parse_command("/buscar    ");
        REQUIRE(result.type == ParsedCommand::Type::SearchEmpty);
        REQUIRE(result.text.empty());
    }
}

TEST_CASE("parse_command no reconoce comandos similares", "[command_parser]") {
    SECTION("/buscarx no es el comando") {
        const auto result = parse_command("/buscarx algo");
        REQUIRE(result.type == ParsedCommand::Type::Normal);
        REQUIRE(result.text == "/buscarx algo");
    }

    SECTION("Texto normal") {
        const auto result = parse_command("hola mundo");
        REQUIRE(result.type == ParsedCommand::Type::Normal);
        REQUIRE(result.text == "hola mundo");
    }

    SECTION("Otro comando con /") {
        const auto result = parse_command("/otro comando");
        REQUIRE(result.type == ParsedCommand::Type::Normal);
        REQUIRE(result.text == "/otro comando");
    }
}

TEST_CASE("parse_command maneja entrada vacía", "[command_parser]") {
    const auto result = parse_command("");
    REQUIRE(result.type == ParsedCommand::Type::Normal);
    REQUIRE(result.text.empty());
}

TEST_CASE("parse_command preserva consultas con caracteres especiales", "[command_parser]") {
    const auto result = parse_command("/buscar ¿Cuál es la capital de México?");
    REQUIRE(result.type == ParsedCommand::Type::Search);
    REQUIRE(result.text == "¿Cuál es la capital de México?");
}
