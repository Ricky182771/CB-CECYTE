#include "chatbot/bot/split.h"
#include "chatbot/bot/types.h"

#include "chatbot/utf8.h"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace {

using chatbot::bot::MessageLimit;
using chatbot::bot::split_markdown;
using chatbot::bot::split_plain;
using chatbot::bot::utf16_length;

MessageLimit bytes(std::size_t max) {
    return {max, {}};
}

MessageLimit utf16(std::size_t max) {
    return {max, utf16_length};
}

/// Una medida rara: cada vocal sin acento cuenta 3 y lo demás como UTF-16.
MessageLimit weird(std::size_t max) {
    return {max, [](std::string_view text) {
                std::size_t size = utf16_length(text);
                for (const char c : text) {
                    if (c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u') {
                        size += 2;
                    }
                }
                return size;
            }};
}

bool is_fence_line(std::string_view line) {
    const std::size_t first = line.find_first_not_of(' ');
    return first != std::string_view::npos &&
           (line.substr(first, 3) == "```" || line.substr(first, 3) == "~~~");
}

/// El contenido sin las líneas de cerca y sin espacios: lo que debe
/// conservarse al partir (las cercas se repiten y los cortes quitan
/// espacios).
std::string content_of(std::string_view text) {
    std::string out;
    std::size_t pos = 0;
    while (pos <= text.size()) {
        const std::size_t newline = text.find('\n', pos);
        const std::size_t end = newline == std::string_view::npos ? text.size() : newline;
        const std::string_view line = text.substr(pos, end - pos);
        if (!is_fence_line(line)) {
            for (const char c : line) {
                if (c != ' ' && c != '\t' && c != '\r') {
                    out.push_back(c);
                }
            }
        }
        if (newline == std::string_view::npos) {
            break;
        }
        pos = newline + 1;
    }
    return out;
}

std::string joined_content(const std::vector<std::string>& pieces) {
    std::string out;
    for (const std::string& piece : pieces) {
        out += content_of(piece);
    }
    return out;
}

bool blank(std::string_view text) {
    return text.find_first_not_of(" \t\r\n") == std::string_view::npos;
}

/// Primera violación de "ningún pedazo pasa del tope, ninguno vacío, todos
/// UTF-8 válido y el contenido se conserva", o "".
std::string check_pieces(std::string_view original, const std::vector<std::string>& pieces,
                         const MessageLimit& limit) {
    for (std::size_t i = 0; i < pieces.size(); ++i) {
        if (limit.size_of(pieces[i]) > limit.max_size) {
            return "el pedazo " + std::to_string(i) + " mide " +
                   std::to_string(limit.size_of(pieces[i]));
        }
        if (blank(pieces[i])) {
            return "el pedazo " + std::to_string(i) + " está vacío";
        }
        if (!chatbot::utf8::is_valid(pieces[i])) {
            return "el pedazo " + std::to_string(i) + " parte un carácter";
        }
    }
    if (joined_content(pieces) != content_of(original)) {
        return "juntos no tienen el mismo contenido";
    }
    return {};
}

/// Cuántas líneas de cerca tiene el texto (pares si los bloques cierran).
std::size_t fence_lines(std::string_view text) {
    std::size_t count = 0;
    std::size_t pos = 0;
    while (pos < text.size()) {
        const std::size_t newline = text.find('\n', pos);
        const std::size_t end = newline == std::string_view::npos ? text.size() : newline;
        count += is_fence_line(text.substr(pos, end - pos)) ? 1 : 0;
        pos = end + 1;
    }
    return count;
}

} // namespace

TEST_CASE("split: una respuesta corta sale en un pedazo", "[bot][split]") {
    const std::string text = "Hola, **mundo**.\n\n- uno\n- dos\n";
    const auto pieces = split_markdown(text, utf16(4096));
    REQUIRE(pieces.size() == 1);
    CHECK(pieces[0] == "Hola, **mundo**.\n\n- uno\n- dos");
}

TEST_CASE("split: sin texto no hay pedazos", "[bot][split]") {
    CHECK(split_markdown("", utf16(100)).empty());
    CHECK(split_markdown("\n  \n\t\n", utf16(100)).empty());
    CHECK(split_plain("   \n", utf16(100)).empty());
}

TEST_CASE("split: corta entre bloques y junta los que caben", "[bot][split]") {
    const std::string a = "Primer párrafo con algo de texto.";  // 33
    const std::string b = "Segundo párrafo, también corto.";    // 31
    const std::string c = "Tercero.";
    const std::string text = a + "\n\n" + b + "\n\n" + c + "\n";
    const auto pieces = split_markdown(text, utf16(70));
    REQUIRE(pieces.size() == 2);
    CHECK(pieces[0] == a + "\n\n" + b);
    CHECK(pieces[1] == c);
}

TEST_CASE("split: un bloque de código más largo que el tope se parte con sus cercas",
          "[bot][split]") {
    std::string code;
    for (int i = 0; i < 40; ++i) {
        code += "print('línea " + std::to_string(i) + "')\n";
    }
    const std::string text = "Mira:\n\n```python\n" + code + "```\n\nListo.";
    const MessageLimit limit = utf16(200);
    const auto pieces = split_markdown(text, limit);
    REQUIRE(pieces.size() > 3);
    CHECK(check_pieces(text, pieces, limit).empty());
    std::string recovered;
    for (const std::string& piece : pieces) {
        if (piece.find("print(") == std::string::npos) {
            continue; // "Mira:" y "Listo." pueden ir solos.
        }
        // Cada pedazo con código es un bloque completo, con su lenguaje.
        const std::size_t open = piece.find("```python\n");
        REQUIRE(open != std::string::npos);
        const std::size_t close = piece.find("\n```", open + 10);
        REQUIRE(close != std::string::npos);
        CHECK(fence_lines(piece) == 2);
        recovered += piece.substr(open + 10, close + 1 - (open + 10));
    }
    CHECK(recovered == code);
}

TEST_CASE("split: la cerca original se repite tal cual (tildes, largo y lenguaje)",
          "[bot][split]") {
    std::string code;
    for (int i = 0; i < 30; ++i) {
        code += "const x" + std::to_string(i) + " = " + std::to_string(i) + ";\n";
    }
    const std::string text = "~~~~js title=\"a\"\n" + code + "~~~~\n";
    const auto pieces = split_markdown(text, bytes(120));
    REQUIRE(pieces.size() > 2);
    for (const std::string& piece : pieces) {
        CHECK(piece.rfind("~~~~js title=\"a\"\n", 0) == 0);
        CHECK(piece.substr(piece.size() - 5) == "\n~~~~");
    }
    CHECK(check_pieces(text, pieces, bytes(120)).empty());
}

TEST_CASE("split: un bloque de código sin cerrar también se parte cerrado", "[bot][split]") {
    std::string code;
    for (int i = 0; i < 30; ++i) {
        code += "echo " + std::to_string(i) + "\n";
    }
    const std::string text = "```bash\n" + code;
    const auto pieces = split_markdown(text, bytes(60));
    REQUIRE(pieces.size() > 2);
    for (const std::string& piece : pieces) {
        CHECK(piece.rfind("```bash\n", 0) == 0);
        CHECK(fence_lines(piece) == 2);
    }
    CHECK(check_pieces(text, pieces, bytes(60)).empty());
}

TEST_CASE("split: al partir un bloque de código no se pierde lo que lo rodea",
          "[bot][split]") {
    // Una definición de enlace antes de la cerca y una nota al pie después
    // caen en el rango del bloque; se conservan fuera de las cercas.
    std::string code;
    for (int i = 0; i < 20; ++i) {
        code += "linea_" + std::to_string(i) + "();\n";
    }
    const std::string text = "[ref]: https://ejemplo.mx\n```c\n" + code +
                             "```\n\n[^1]: Una nota al pie.\n\nVer [ref] y nota[^1].";
    const MessageLimit limit = bytes(80);
    const auto pieces = split_markdown(text, limit);
    CHECK(check_pieces(text, pieces, limit).empty());
    for (const std::string& piece : pieces) {
        INFO(piece);
        CHECK(fence_lines(piece) % 2 == 0);
    }
}

TEST_CASE("split: una línea de código más larga que el tope", "[bot][split]") {
    const std::string line(500, 'x');
    const std::string text = "```\n" + line + "\ncorta\n```";
    const MessageLimit limit = bytes(64);
    const auto pieces = split_markdown(text, limit);
    CHECK(check_pieces(text, pieces, limit).empty());
    for (const std::string& piece : pieces) {
        CHECK(fence_lines(piece) == 2);
    }
}

TEST_CASE("split: un párrafo sin espacios más largo que el tope", "[bot][split]") {
    const std::string word(1000, 'a');
    const MessageLimit limit = utf16(100);
    const auto pieces = split_markdown(word, limit);
    REQUIRE(pieces.size() == 10);
    CHECK(check_pieces(word, pieces, limit).empty());
}

TEST_CASE("split: un párrafo largo se corta en un espacio", "[bot][split]") {
    std::string text;
    for (int i = 0; i < 200; ++i) {
        text += "palabra" + std::to_string(i) + " ";
    }
    const MessageLimit limit = utf16(90);
    const auto pieces = split_markdown(text, limit);
    REQUIRE(pieces.size() > 10);
    CHECK(check_pieces(text, pieces, limit).empty());
    // Ninguna palabra quedó partida: cada pedazo empieza y termina en una.
    for (const std::string& piece : pieces) {
        CHECK(piece.front() == 'p');
        CHECK(piece.back() != ' ');
    }
    std::string words_only;
    for (const std::string& piece : pieces) {
        words_only += piece + " ";
    }
    CHECK(words_only == text);
}

TEST_CASE("split: acentos y emoji con la medida UTF-16", "[bot][split]") {
    std::string text;
    for (int i = 0; i < 60; ++i) {
        text += "ñandú 👍🏽 ";
    }
    const MessageLimit limit = utf16(37);
    const auto pieces = split_markdown(text, limit);
    CHECK(check_pieces(text, pieces, limit).empty());

    SECTION("sin espacios: no separa el tono de piel de su emoji") {
        std::string emoji;
        for (int i = 0; i < 50; ++i) {
            emoji += "👍🏽"; // 4 unidades UTF-16.
        }
        const MessageLimit odd = utf16(17);
        const auto parts = split_markdown(emoji, odd);
        CHECK(check_pieces(emoji, parts, odd).empty());
        for (const std::string& part : parts) {
            CHECK(part.rfind("🏽", 0) != 0);
        }
    }
}

TEST_CASE("split: una medida rara", "[bot][split]") {
    std::string text = "# Título\n\n";
    for (int i = 0; i < 30; ++i) {
        text += "Una oración más con vocales " + std::to_string(i) + ".\n";
    }
    text += "\n```\nbloque de código con vocales\n```\n";
    const MessageLimit limit = weird(80);
    CHECK(check_pieces(text, split_markdown(text, limit), limit).empty());
}

TEST_CASE("split: texto plano por líneas", "[bot][split]") {
    const std::string text = "Fuentes:\n[1] Uno\nhttps://uno.example\n[2] Dos\nhttps://dos.example";
    const auto pieces = split_plain(text, bytes(40));
    REQUIRE(pieces.size() == 2);
    CHECK(pieces[0] == "Fuentes:\n[1] Uno\nhttps://uno.example");
    CHECK(pieces[1] == "[2] Dos\nhttps://dos.example");
}

TEST_CASE("split: un tope menor que el mínimo se trata como el mínimo", "[bot][split]") {
    const auto pieces = split_plain("abcdefghijklmnopqrstuvwxyz", bytes(3));
    REQUIRE(pieces.size() == 2);
    CHECK(pieces[0].size() == chatbot::bot::kMinMessageLimit);
}

TEST_CASE("split: propiedades con textos aleatorios", "[bot][split]") {
    std::mt19937 random{4096};
    const auto pick = [&](std::size_t n) {
        return std::uniform_int_distribution<std::size_t>(0, n - 1)(random);
    };
    const std::vector<std::string> words = {"hola", "año",   "ñandú",  "código", "👍🏽",
                                            "中文",  "x",     "función", "la",     "palabra",
                                            "😀",    "árbol", "é"};
    const auto sentence = [&](std::size_t count, bool spaces) {
        std::string out;
        for (std::size_t i = 0; i < count; ++i) {
            if (i > 0 && spaces) {
                out += ' ';
            }
            out += words[pick(words.size())];
        }
        return out;
    };
    for (int round = 0; round < 400; ++round) {
        std::string text;
        const std::size_t blocks = 1 + pick(8);
        for (std::size_t b = 0; b < blocks; ++b) {
            switch (pick(6)) {
            case 0:
                text += sentence(1 + pick(80), true);
                break;
            case 1:
                text += sentence(1 + pick(60), false); // Sin espacios.
                break;
            case 2: {
                text += "```" + std::string{pick(2) == 0 ? "cpp" : ""} + "\n";
                const std::size_t lines = 1 + pick(30);
                for (std::size_t i = 0; i < lines; ++i) {
                    text += "    " + sentence(pick(12), pick(4) != 0) + "\n";
                }
                text += "```";
                break;
            }
            case 3:
                for (std::size_t i = 0, n = 1 + pick(10); i < n; ++i) {
                    text += (i > 0 ? "\n- " : "- ") + sentence(1 + pick(15), true);
                }
                break;
            case 4:
                text += "| a | b |\n|---|---|";
                for (std::size_t i = 0, n = 1 + pick(10); i < n; ++i) {
                    text += "\n| " + sentence(1 + pick(4), true) + " | " + sentence(1, true) +
                            " |";
                }
                break;
            default:
                text += "## " + sentence(1 + pick(6), true);
                break;
            }
            text += "\n\n";
        }
        const std::size_t max = 16 + pick(300);
        const MessageLimit limit = pick(3) == 0 ? bytes(max) : (pick(2) == 0 ? utf16(max) : weird(max));
        INFO("ronda " << round << ", tope " << max << ":\n" << text);
        const auto pieces = split_markdown(text, limit);
        REQUIRE(check_pieces(text, pieces, limit).empty());
        for (const std::string& piece : pieces) {
            // Los bloques están separados por líneas en blanco: ningún
            // pedazo deja una cerca abierta.
            REQUIRE(fence_lines(piece) % 2 == 0);
        }
    }
}
