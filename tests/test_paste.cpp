#include <catch2/catch_test_macros.hpp>

#include "paste.h"

#include "chatbot/utf8.h"

#include <string>

using chatbot::cli::kMaxPasteBytes;
using chatbot::cli::sanitize_paste;

TEST_CASE("sanitize_paste convierte CRLF y CR en LF", "[paste]") {
    CHECK(sanitize_paste("a\r\nb\rc\nd", true).text == "a\nb\nc\nd");
    CHECK(sanitize_paste("a\r\r\nb", true).text == "a\n\nb");
    CHECK(sanitize_paste("fin\r", true).text == "fin\n");
}

TEST_CASE("sanitize_paste cambia los tabuladores por 4 espacios", "[paste]") {
    CHECK(sanitize_paste("\tx = 1;\n\t\ty", true).text == "    x = 1;\n        y");
}

TEST_CASE("sanitize_paste quita controles C0, DEL y C1", "[paste]") {
    SECTION("C0 salvo los saltos y el tabulador") {
        CHECK(sanitize_paste(std::string{"a\x01\x07\x08\x1B[31mb\x1F"}, true).text == "a[31mb");
        CHECK(sanitize_paste(std::string{"x\0y", 3}, true).text == "xy");
    }
    SECTION("DEL") {
        CHECK(sanitize_paste("a\x7F" "b", true).text == "ab");
    }
    SECTION("C1 (U+0080 a U+009F), pero no U+00A0") {
        CHECK(sanitize_paste("a\xC2\x80\xC2\x9B" "b\xC2\x9F", true).text == "ab");
        CHECK(sanitize_paste("a\xC2\xA0" "b", true).text == "a\xC2\xA0" "b");
    }
    SECTION("El texto normal queda igual") {
        const std::string text = "¿Qué tal? ñandú — 日本 😀 {x}";
        CHECK(sanitize_paste(text, true).text == text);
        CHECK_FALSE(sanitize_paste(text, true).truncated);
    }
}

TEST_CASE("sanitize_paste sustituye el UTF-8 inválido", "[paste]") {
    const std::string r{chatbot::utf8::kReplacement};
    SECTION("Un byte suelto") {
        CHECK(sanitize_paste("a\xFF" "b", true).text == "a" + r + "b");
    }
    SECTION("Una secuencia incompleta al final: un U+FFFD por byte") {
        CHECK(sanitize_paste("a\xE2\x82", true).text == "a" + r + r);
    }
    SECTION("Sobrelarga y sustitutos") {
        CHECK(sanitize_paste("\xC0\xAF", true).text == r + r);
        CHECK(sanitize_paste("\xED\xA0\x80", true).text == r + r + r);
    }
    SECTION("El resultado siempre es UTF-8 válido") {
        CHECK(chatbot::utf8::is_valid(sanitize_paste("\x80\xBF\xF8\xC3", true).text));
    }
}

TEST_CASE("sanitize_paste en una línea", "[paste]") {
    SECTION("Quita los saltos del final (una key copiada de una página)") {
        CHECK(sanitize_paste("nvapi-abc123\n", false).text == "nvapi-abc123");
        CHECK(sanitize_paste("nvapi-abc123\r\n\r\n", false).text == "nvapi-abc123");
    }
    SECTION("Quita también los del principio") {
        CHECK(sanitize_paste("\n\nabc", false).text == "abc");
    }
    SECTION("Los de en medio pasan a un espacio") {
        CHECK(sanitize_paste("uno\ndos\r\ntres", false).text == "uno dos tres");
        CHECK(sanitize_paste("a\n\nb", false).text == "a  b");
    }
    SECTION("Solo saltos: vacío") {
        CHECK(sanitize_paste("\n\r\n", false).text.empty());
    }
    SECTION("Con varias líneas se conservan") {
        CHECK(sanitize_paste("\nabc\n", true).text == "\nabc\n");
    }
}

TEST_CASE("sanitize_paste recorta a kMaxPasteBytes sin partir un carácter", "[paste]") {
    SECTION("Justo en el tope no recorta") {
        const std::string text(kMaxPasteBytes, 'a');
        const auto paste = sanitize_paste(text, true);
        CHECK(paste.text.size() == kMaxPasteBytes);
        CHECK_FALSE(paste.truncated);
    }
    SECTION("Un byte de más recorta") {
        const std::string text(kMaxPasteBytes + 1, 'a');
        const auto paste = sanitize_paste(text, true);
        CHECK(paste.text.size() == kMaxPasteBytes);
        CHECK(paste.truncated);
    }
    SECTION("Un carácter de 4 bytes que no cabe completo queda fuera") {
        // Faltan 2 bytes para el tope: el emoji (4 bytes) no cabe.
        const std::string text = std::string(kMaxPasteBytes - 2, 'a') + "\xF0\x9F\x98\x80" + "b";
        const auto paste = sanitize_paste(text, true);
        CHECK(paste.truncated);
        CHECK(paste.text.size() == kMaxPasteBytes - 2);
        CHECK(chatbot::utf8::is_valid(paste.text));
    }
    SECTION("Un tabulador que ya no cabe tampoco se parte") {
        const std::string text = std::string(kMaxPasteBytes - 3, 'a') + "\t";
        const auto paste = sanitize_paste(text, true);
        CHECK(paste.truncated);
        CHECK(paste.text.size() == kMaxPasteBytes - 3);
    }
}
