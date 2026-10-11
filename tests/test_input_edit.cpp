#include <catch2/catch_test_macros.hpp>

#include "input_edit.h"

#include <string>

using chatbot::cli::backslash_newline;
using chatbot::cli::input_height;
using chatbot::cli::input_placeholder;
using chatbot::cli::insert_at_cursor;

TEST_CASE("\\ + Enter cambia la diagonal antes del cursor por un salto", "[input_edit]") {
    SECTION("Al final del texto") {
        std::string text = "hola\\";
        int cursor = 5;
        CHECK(backslash_newline(text, cursor));
        CHECK(text == "hola\n");
        CHECK(cursor == 5);
    }
    SECTION("A la mitad: el cursor justo después de la diagonal") {
        std::string text = "uno\\dos";
        int cursor = 4;
        CHECK(backslash_newline(text, cursor));
        CHECK(text == "uno\ndos");
        CHECK(cursor == 4);
    }
    SECTION("La diagonal no está antes del cursor: Enter envía") {
        std::string text = "uno\\dos";
        int cursor = 7;
        CHECK_FALSE(backslash_newline(text, cursor));
        CHECK(text == "uno\\dos");
        CHECK(cursor == 7);

        cursor = 3; // Antes de la diagonal.
        CHECK_FALSE(backslash_newline(text, cursor));
        CHECK(text == "uno\\dos");
    }
    SECTION("Con \\\\ solo se consume la última") {
        std::string text = "ruta\\\\";
        int cursor = 6;
        CHECK(backslash_newline(text, cursor));
        CHECK(text == "ruta\\\n");
        CHECK(cursor == 6);
    }
    SECTION("Caja vacía o cursor al principio") {
        std::string empty;
        int cursor = 0;
        CHECK_FALSE(backslash_newline(empty, cursor));
        std::string text = "\\x";
        CHECK_FALSE(backslash_newline(text, cursor));
        CHECK(text == "\\x");
    }
    SECTION("Un cursor fuera de rango se acota") {
        std::string text = "fin\\";
        int cursor = 99;
        CHECK(backslash_newline(text, cursor));
        CHECK(text == "fin\n");
        CHECK(cursor == 4);
        cursor = -3;
        CHECK_FALSE(backslash_newline(text, cursor));
    }
}

TEST_CASE("insert_at_cursor inserta y deja el cursor después", "[input_edit]") {
    std::string text = "ab";
    int cursor = 1;
    insert_at_cursor(text, cursor, "\n");
    CHECK(text == "a\nb");
    CHECK(cursor == 2);
    insert_at_cursor(text, cursor, "ñ");
    CHECK(text == "a\nñb");
    CHECK(cursor == 4);
    cursor = 50;
    insert_at_cursor(text, cursor, "!");
    CHECK(text == "a\nñb!");
    CHECK(cursor == 6);
    CHECK(chatbot::cli::cursor_at_end(text) == 6);
}

TEST_CASE("input_height crece con las líneas hasta el tope", "[input_edit]") {
    SECTION("Una línea, o vacía") {
        CHECK(input_height("", 40) == 1);
        CHECK(input_height("hola", 40) == 1);
    }
    SECTION("Una fila por línea") {
        CHECK(input_height("a\nb", 40) == 2);
        CHECK(input_height("a\nb\nc\n", 40) == 4);
    }
    SECTION("Hasta 8 líneas") {
        CHECK(input_height(std::string(7, '\n'), 40) == 8);
        CHECK(input_height(std::string(30, '\n'), 40) == 8);
    }
    SECTION("O un tercio de la terminal, lo que sea menor") {
        CHECK(input_height(std::string(30, '\n'), 24) == 8);
        CHECK(input_height(std::string(30, '\n'), 18) == 6);
        CHECK(input_height(std::string(30, '\n'), 10) == 3);
    }
    SECTION("Siempre al menos una fila") {
        CHECK(input_height(std::string(30, '\n'), 2) == 1);
        CHECK(input_height("a\nb", 0) == 1);
    }
}

TEST_CASE("input_placeholder cabe en el ancho", "[input_edit]") {
    const std::string full =
        "Escribe tu mensaje · Enter envía · \\ + Enter: nueva línea · / comandos";
    const std::string medium = "Escribe tu mensaje · \\ + Enter: nueva línea · / comandos";
    const std::string short_commands = "Mensaje · \\ + Enter: nueva línea · / comandos";
    const std::string short_newline = "Mensaje · \\ + Enter: nueva línea";
    CHECK(input_placeholder(120) == full);
    CHECK(input_placeholder(70) == full);
    CHECK(input_placeholder(69) == medium);
    CHECK(input_placeholder(56) == medium);
    CHECK(input_placeholder(55) == short_commands);
    CHECK(input_placeholder(45) == short_commands);
    CHECK(input_placeholder(44) == short_newline);
    CHECK(input_placeholder(32) == short_newline);
    CHECK(input_placeholder(31) == "Escribe tu mensaje");
    CHECK(input_placeholder(5) == "Escribe tu mensaje");
}
