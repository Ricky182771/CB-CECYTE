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

TEST_CASE("parse_command reconoce /copiar", "[command_parser]") {
    SECTION("Sin número: el último bloque") {
        const auto result = parse_command("/copiar");
        REQUIRE(result.type == ParsedCommand::Type::Copy);
        CHECK_FALSE(result.block.has_value());
    }

    SECTION("Con número y espacios de más") {
        const auto result = parse_command("  /copiar   3  ");
        REQUIRE(result.type == ParsedCommand::Type::Copy);
        REQUIRE(result.block == 3);
    }

    SECTION("Un número fuera de rango lo revisa quien lo usa") {
        const auto zero = parse_command("/copiar 0");
        REQUIRE(zero.type == ParsedCommand::Type::Copy);
        CHECK(zero.block == 0);
        const auto big = parse_command("/copiar 999999999");
        REQUIRE(big.type == ParsedCommand::Type::Copy);
        CHECK(big.block == 999999999);
    }
}

TEST_CASE("parse_command detecta errores de uso de /copiar", "[command_parser]") {
    for (const char* input : {"/copiar tres", "/copiar 3 4", "/copiar -1", "/copiar 3x",
                              "/copiar 1234567890"}) {
        INFO(input);
        const auto result = parse_command(input);
        CHECK(result.type == ParsedCommand::Type::CopyUsage);
        CHECK_FALSE(result.block.has_value());
    }
}

TEST_CASE("parse_command reconoce /guardar", "[command_parser]") {
    SECTION("Sin argumentos") {
        const auto result = parse_command("/guardar");
        REQUIRE(result.type == ParsedCommand::Type::Save);
        CHECK_FALSE(result.block.has_value());
        CHECK(result.file_name.empty());
    }

    SECTION("Con número") {
        const auto result = parse_command("/guardar 3");
        REQUIRE(result.type == ParsedCommand::Type::Save);
        CHECK(result.block == 3);
        CHECK(result.file_name.empty());
    }

    SECTION("Con número y nombre") {
        const auto result = parse_command("/guardar 3 suma.cpp");
        REQUIRE(result.type == ParsedCommand::Type::Save);
        CHECK(result.block == 3);
        CHECK(result.file_name == "suma.cpp");
    }

    SECTION("El nombre se pasa sin validar") {
        const auto result = parse_command("/guardar 1 ../x");
        REQUIRE(result.type == ParsedCommand::Type::Save);
        CHECK(result.file_name == "../x");
    }
}

TEST_CASE("parse_command detecta errores de uso de /guardar", "[command_parser]") {
    for (const char* input : {"/guardar suma.cpp", "/guardar 3 suma.cpp extra",
                              "/guardar 3 mi archivo.cpp", "/guardar uno dos"}) {
        INFO(input);
        const auto result = parse_command(input);
        CHECK(result.type == ParsedCommand::Type::SaveUsage);
        CHECK_FALSE(result.block.has_value());
        CHECK(result.file_name.empty());
    }
}

TEST_CASE("parse_command reconoce /exportar", "[command_parser]") {
    CHECK(parse_command("/exportar").type == ParsedCommand::Type::Export);
    CHECK(parse_command("  /exportar  ").type == ParsedCommand::Type::Export);
    CHECK(parse_command("/exportar todo").type == ParsedCommand::Type::ExportUsage);
}

TEST_CASE("parse_command manda como mensaje normal los comandos parecidos", "[command_parser]") {
    for (const char* input : {"/copiarx", "/copiarx 3", "/guardarlo", "/exportarlo", "/Copiar",
                              "copiar 3", "/copia"}) {
        INFO(input);
        const auto result = parse_command(input);
        CHECK(result.type == ParsedCommand::Type::Normal);
        CHECK(result.text == input);
    }
}

TEST_CASE("parse_command acepta saltos de línea entre argumentos", "[command_parser]") {
    SECTION("/copiar y /guardar") {
        const auto copy = parse_command("/copiar\n2");
        REQUIRE(copy.type == ParsedCommand::Type::Copy);
        REQUIRE(copy.block == 2);

        const auto save = parse_command("/guardar 3\nnotas.txt\n");
        REQUIRE(save.type == ParsedCommand::Type::Save);
        REQUIRE(save.block == 3);
        REQUIRE(save.file_name == "notas.txt");

        REQUIRE(parse_command("/guardar\n3\nuno\ndos").type == ParsedCommand::Type::SaveUsage);
    }
    SECTION("/exportar con un salto final sigue sin argumentos") {
        REQUIRE(parse_command("/exportar\n").type == ParsedCommand::Type::Export);
        REQUIRE(parse_command("/exportar\nalgo").type == ParsedCommand::Type::ExportUsage);
    }
    SECTION("/buscar seguido de un salto de línea") {
        const auto result = parse_command("/buscar\nclima en Toluca");
        REQUIRE(result.type == ParsedCommand::Type::Search);
        REQUIRE(result.text == "clima en Toluca");
        REQUIRE(parse_command("/buscar\n\n").type == ParsedCommand::Type::SearchEmpty);
    }
    SECTION("La consulta de /buscar va en una línea") {
        const auto result = parse_command("/buscar receta de\nmole\npoblano\n");
        REQUIRE(result.type == ParsedCommand::Type::Search);
        REQUIRE(result.text == "receta de mole poblano");
    }
    SECTION("Un mensaje normal conserva sus saltos") {
        const auto result = parse_command("hola\n\nmundo");
        REQUIRE(result.type == ParsedCommand::Type::Normal);
        REQUIRE(result.text == "hola\n\nmundo");
    }
}
