#include "chatbot/markdown/markdown.h"
#include "chatbot/markdown/source_blocks.h"
#include "performance.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace {

using chatbot::md::Block;
using chatbot::md::parse;
using chatbot::md::source_blocks;
using chatbot::md::SourceBlock;
using Kind = chatbot::md::Block::Kind;

std::string_view text_of(std::string_view markdown, const SourceBlock& block) {
    return markdown.substr(block.begin, block.end - block.begin);
}

/// Primera violación del contrato de source_blocks, o "" si lo cumple:
/// rangos seguidos que cubren todo el texto y empiezan en una línea, los
/// mismos bloques (en número y tipo) que parse() y cada rango, parseado
/// solo, con exactamente sus bloques.
std::string check_contract(std::string_view markdown) {
    const std::vector<SourceBlock> ranges = source_blocks(markdown);
    const std::vector<Block> blocks = parse(markdown).blocks;
    if (blocks.empty()) {
        return ranges.empty() ? "" : "rangos sin bloques";
    }
    if (ranges.empty()) {
        return "sin rangos";
    }
    if (ranges.front().begin != 0 || ranges.back().end != markdown.size()) {
        return "los rangos no cubren todo el texto";
    }
    std::size_t index = 0;
    for (std::size_t i = 0; i < ranges.size(); ++i) {
        const SourceBlock& range = ranges[i];
        if (i > 0 && range.begin != ranges[i - 1].end) {
            return "rango " + std::to_string(i) + " no sigue al anterior";
        }
        if (range.begin >= range.end) {
            return "rango " + std::to_string(i) + " vacío";
        }
        if (range.begin > 0 && markdown[range.begin - 1] != '\n') {
            return "rango " + std::to_string(i) + " no empieza en una línea";
        }
        if (range.blocks == 0 || index + range.blocks > blocks.size()) {
            return "rango " + std::to_string(i) + " con bloques de más";
        }
        if (range.kind != blocks[index].kind) {
            return "rango " + std::to_string(i) + " con otro tipo";
        }
        const std::vector<Block> alone = parse(text_of(markdown, range)).blocks;
        if (alone.size() != range.blocks) {
            return "rango " + std::to_string(i) + " parseado solo da " +
                   std::to_string(alone.size()) + " bloques";
        }
        for (std::size_t k = 0; k < alone.size(); ++k) {
            if (alone[k].kind != blocks[index + k].kind) {
                return "rango " + std::to_string(i) + " parseado solo cambia de tipo";
            }
        }
        if (range.fence.has_value() &&
            !(range.begin <= range.fence->open_begin &&
              range.fence->open_begin < range.fence->content_begin &&
              range.fence->content_begin <= range.fence->content_end &&
              range.fence->content_end <= range.end)) {
            return "rango " + std::to_string(i) + " con la cerca fuera de lugar";
        }
        index += range.blocks;
    }
    return index == blocks.size() ? "" : "faltan bloques";
}

/// Tipos de los rangos, para comparar de un vistazo.
std::vector<Kind> kinds(const std::vector<SourceBlock>& ranges) {
    std::vector<Kind> out;
    for (const SourceBlock& range : ranges) {
        out.push_back(range.kind);
    }
    return out;
}

/// Textos de los rangos.
std::vector<std::string> texts(std::string_view markdown, const std::vector<SourceBlock>& ranges) {
    std::vector<std::string> out;
    for (const SourceBlock& range : ranges) {
        out.emplace_back(text_of(markdown, range));
    }
    return out;
}

} // namespace

TEST_CASE("source_blocks: sin bloques, sin rangos", "[markdown][source_blocks]") {
    CHECK(source_blocks("").empty());
    CHECK(source_blocks("\n\n   \n").empty());
    // Una definición de enlace sola no es un bloque.
    CHECK(source_blocks("[x]: https://ejemplo.mx\n").empty());
}

TEST_CASE("source_blocks: párrafos; las líneas en blanco van con el anterior",
          "[markdown][source_blocks]") {
    const std::string markdown = "Hola mundo.\n\nSegundo\npárrafo.\n";
    const auto ranges = source_blocks(markdown);
    CHECK(kinds(ranges) == std::vector<Kind>{Kind::Paragraph, Kind::Paragraph});
    CHECK(texts(markdown, ranges) ==
          std::vector<std::string>{"Hola mundo.\n\n", "Segundo\npárrafo.\n"});
    CHECK(check_contract(markdown).empty());
}

TEST_CASE("source_blocks: las líneas en blanco del principio van con el primero",
          "[markdown][source_blocks]") {
    const std::string markdown = "\n\n[x]: https://ejemplo.mx\n\nTexto [x].";
    const auto ranges = source_blocks(markdown);
    REQUIRE(ranges.size() == 1);
    CHECK(ranges[0].kind == Kind::Paragraph);
    CHECK(text_of(markdown, ranges[0]) == markdown);
}

TEST_CASE("source_blocks: títulos ATX y setext", "[markdown][source_blocks]") {
    const std::string markdown = "# Título\nTexto\n\nOtro título\n======\nFin\n";
    const auto ranges = source_blocks(markdown);
    CHECK(kinds(ranges) ==
          std::vector<Kind>{Kind::Heading, Kind::Paragraph, Kind::Heading, Kind::Paragraph});
    // El subrayado es del título, no del párrafo que sigue.
    CHECK(texts(markdown, ranges) ==
          std::vector<std::string>{"# Título\n", "Texto\n\n", "Otro título\n======\n", "Fin\n"});
    CHECK(check_contract(markdown).empty());
}

TEST_CASE("source_blocks: código con lenguaje entre párrafos, sin líneas en blanco",
          "[markdown][source_blocks]") {
    const std::string markdown = "Texto\n```cpp\nint x;\n\nint y;\n```\nMás\n";
    const auto ranges = source_blocks(markdown);
    REQUIRE(kinds(ranges) == std::vector<Kind>{Kind::Paragraph, Kind::Code, Kind::Paragraph});
    CHECK(texts(markdown, ranges) ==
          std::vector<std::string>{"Texto\n", "```cpp\nint x;\n\nint y;\n```\n", "Más\n"});
    const SourceBlock& code = ranges[1];
    REQUIRE(code.fence.has_value());
    CHECK(code.fence->fence == "```");
    CHECK(code.fence->open_begin == markdown.find("```cpp"));
    CHECK(markdown.substr(code.fence->content_begin,
                          code.fence->content_end - code.fence->content_begin) ==
          "int x;\n\nint y;\n");
    CHECK(check_contract(markdown).empty());
}

TEST_CASE("source_blocks: código sin lenguaje, con tildes y vacío", "[markdown][source_blocks]") {
    SECTION("sin lenguaje: la cerca no tiene texto") {
        const std::string markdown = "Antes\n\n```\n\nsin lenguaje\n```\n\nDespués\n";
        const auto ranges = source_blocks(markdown);
        REQUIRE(kinds(ranges) == std::vector<Kind>{Kind::Paragraph, Kind::Code, Kind::Paragraph});
        CHECK(texts(markdown, ranges)[1] == "```\n\nsin lenguaje\n```\n\n");
        REQUIRE(ranges[1].fence.has_value());
        CHECK(markdown.substr(ranges[1].fence->content_begin,
                              ranges[1].fence->content_end - ranges[1].fence->content_begin) ==
              "\nsin lenguaje\n");
    }
    SECTION("tildes: solo cierra una cerca igual o más larga") {
        const std::string markdown = "~~~~python\nprint(1)\n~~~\n~~~~\n";
        const auto ranges = source_blocks(markdown);
        REQUIRE(ranges.size() == 1);
        REQUIRE(ranges[0].fence.has_value());
        CHECK(ranges[0].fence->fence == "~~~~");
        CHECK(ranges[0].fence->content_end == markdown.rfind("~~~~"));
    }
    SECTION("vacío: un bloque sin ningún texto") {
        const std::string markdown = "Antes\n\n```\n```\n\nDespués\n";
        const auto ranges = source_blocks(markdown);
        REQUIRE(kinds(ranges) == std::vector<Kind>{Kind::Paragraph, Kind::Code, Kind::Paragraph});
        CHECK(texts(markdown, ranges) ==
              std::vector<std::string>{"Antes\n\n", "```\n```\n\n", "Después\n"});
        CHECK(check_contract(markdown).empty());
    }
    SECTION("dos seguidos") {
        const std::string markdown = "```\na\n```\n```\nb\n```";
        const auto ranges = source_blocks(markdown);
        CHECK(texts(markdown, ranges) == std::vector<std::string>{"```\na\n```\n", "```\nb\n```"});
    }
    SECTION("sin cerrar: el contenido llega hasta el final") {
        const std::string markdown = "Texto\n\n```js\nconsole.log(1)\n";
        const auto ranges = source_blocks(markdown);
        REQUIRE(ranges.size() == 2);
        REQUIRE(ranges[1].fence.has_value());
        CHECK(ranges[1].fence->content_end == markdown.size());
    }
}

TEST_CASE("source_blocks: lo que sigue a la cerca que cierra no es contenido",
          "[markdown][source_blocks]") {
    // La nota al pie no es un bloque: queda en el rango del código, pero la
    // cerca que cierra es la primera, no la última línea del rango.
    const std::string markdown = "```\ncódigo\n```\n\n[^1]: nota\n\nTexto[^1].\n";
    const auto ranges = source_blocks(markdown);
    REQUIRE(kinds(ranges) == std::vector<Kind>{Kind::Code, Kind::Paragraph});
    CHECK(texts(markdown, ranges)[0] == "```\ncódigo\n```\n\n[^1]: nota\n\n");
    REQUIRE(ranges[0].fence.has_value());
    const auto& fence = *ranges[0].fence;
    CHECK(markdown.substr(fence.content_begin, fence.content_end - fence.content_begin) ==
          "código\n");
    CHECK(markdown.substr(fence.content_end, fence.close_end - fence.content_end) == "```\n");
    CHECK(check_contract(markdown).empty());
}

TEST_CASE("source_blocks: código con sangría no tiene cerca", "[markdown][source_blocks]") {
    const std::string markdown = "Texto\n\n    código\n    más\n\nFin\n";
    const auto ranges = source_blocks(markdown);
    REQUIRE(kinds(ranges) == std::vector<Kind>{Kind::Paragraph, Kind::Code, Kind::Paragraph});
    CHECK_FALSE(ranges[1].fence.has_value());
    CHECK(texts(markdown, ranges)[1] == "    código\n    más\n\n");
}

TEST_CASE("source_blocks: listas, también con un elemento vacío al principio",
          "[markdown][source_blocks]") {
    const std::string markdown = "Texto\n\n-\n- uno\n- dos\n\n1. primero\n2. segundo\n";
    const auto ranges = source_blocks(markdown);
    CHECK(kinds(ranges) ==
          std::vector<Kind>{Kind::Paragraph, Kind::BulletList, Kind::OrderedList});
    CHECK(texts(markdown, ranges) ==
          std::vector<std::string>{"Texto\n\n", "-\n- uno\n- dos\n\n", "1. primero\n2. segundo\n"});
    CHECK(check_contract(markdown).empty());
}

TEST_CASE("source_blocks: un bloque de código dentro de una lista es parte de la lista",
          "[markdown][source_blocks]") {
    const std::string markdown =
        "1. Paso:\n   ```bash\n   ls\n\n   pwd\n   ```\n2. Otro\n\nFin\n";
    const auto ranges = source_blocks(markdown);
    REQUIRE(kinds(ranges) == std::vector<Kind>{Kind::OrderedList, Kind::Paragraph});
    CHECK(texts(markdown, ranges)[1] == "Fin\n");
    CHECK_FALSE(ranges[0].fence.has_value());
}

TEST_CASE("source_blocks: tablas, con y sin filas", "[markdown][source_blocks]") {
    SECTION("con filas") {
        const std::string markdown = "| a | b |\n|---|---|\n| 1 | 2 |\n\nFin\n";
        const auto ranges = source_blocks(markdown);
        CHECK(kinds(ranges) == std::vector<Kind>{Kind::Table, Kind::Paragraph});
        CHECK(texts(markdown, ranges)[0] == "| a | b |\n|---|---|\n| 1 | 2 |\n\n");
    }
    SECTION("solo el encabezado: la fila |---| es de la tabla") {
        const std::string markdown = "| a | b |\n|---|---|\n\nFin\n";
        const auto ranges = source_blocks(markdown);
        CHECK(kinds(ranges) == std::vector<Kind>{Kind::Table, Kind::Paragraph});
        CHECK(texts(markdown, ranges)[0] == "| a | b |\n|---|---|\n\n");
    }
}

TEST_CASE("source_blocks: citas, alertas y reglas", "[markdown][source_blocks]") {
    const std::string markdown = "> cita\n> sigue\n\n> [!NOTE]\n> nota\n\n---\n\ndos\n";
    const auto ranges = source_blocks(markdown);
    CHECK(kinds(ranges) ==
          std::vector<Kind>{Kind::Quote, Kind::Alert, Kind::Rule, Kind::Paragraph});
    CHECK(texts(markdown, ranges) == std::vector<std::string>{"> cita\n> sigue\n\n",
                                                              "> [!NOTE]\n> nota\n\n", "---\n\n",
                                                              "dos\n"});
}

TEST_CASE("source_blocks: una nota al pie va con el bloque anterior",
          "[markdown][source_blocks]") {
    const std::string markdown = "Texto[^1].\n\n[^1]: La nota.\n\nOtro\n";
    const auto ranges = source_blocks(markdown);
    CHECK(kinds(ranges) == std::vector<Kind>{Kind::Paragraph, Kind::Paragraph});
    CHECK(texts(markdown, ranges)[1] == "Otro\n");
    CHECK(check_contract(markdown).empty());
}

TEST_CASE("source_blocks: muestra con todos los elementos", "[markdown][source_blocks]") {
    // La misma muestra que dibujan las pruebas de la vista.
    const std::string markdown =
        "# Muestra\n\nPárrafo con **negritas**, *cursivas*, `código` y un "
        "[enlace](https://ejemplo.mx).\n\n## Lista\n\n- uno\n- dos\n  - anidado\n\n"
        "1. primero\n2. segundo\n\n- [x] hecha\n- [ ] pendiente\n\n> Una cita\n\n"
        "> [!WARNING]\n> Cuidado\n\n```python\nprint('hola')\n```\n\n| a | b |\n|:--|--:|\n"
        "| 1 | 2 |\n\n***\n\nTexto con nota[^n] y $x^2$.\n\n[^n]: La nota.\n";
    CHECK(check_contract(markdown).empty());
    for (const SourceBlock& range : source_blocks(markdown)) {
        CHECK(range.blocks == 1);
    }
}

TEST_CASE("source_blocks: propiedades con documentos aleatorios", "[markdown][source_blocks]") {
    const std::vector<std::string> pieces = {
        "Un párrafo con *énfasis* y `código`.",
        "Otro párrafo\nen dos líneas.",
        "# Título",
        "Subtítulo\n---------",
        "- uno\n- dos\n- tres",
        "1. primero\n2. segundo",
        "```cpp\nint main() {\n\n    return 0;\n}\n```",
        "```\nsin lenguaje\n```",
        "~~~\ntilde\n~~~",
        "    sangría\n    de código",
        "| a | b |\n|---|---|\n| 1 | 2 |",
        "| solo | encabezado |\n|---|---|",
        "> cita\n> de dos líneas",
        "> [!TIP]\n> consejo",
        "---",
        "-\n- con vacío",
        "```\n```",
        "Nota[^1].\n\n[^1]: Texto de la nota.",
        "[ref]: https://ejemplo.mx",
        "Emoji 👍🏽 y acentos: ñandú.",
    };
    const std::vector<std::string> separators = {"\n", "\n\n", "\n\n\n", "\n \n"};
    std::mt19937 random{20261017};
    std::uniform_int_distribution<std::size_t> piece_index(0, pieces.size() - 1);
    std::uniform_int_distribution<std::size_t> separator_index(0, separators.size() - 1);
    std::uniform_int_distribution<int> count(1, 12);
    for (int round = 0; round < 1500; ++round) {
        // Con líneas en blanco entre los pedazos, cada bloque se puede separar.
        const bool blank_only = round % 2 == 0;
        std::string markdown;
        const int n = count(random);
        for (int i = 0; i < n; ++i) {
            if (i > 0) {
                markdown += blank_only ? std::string{"\n\n"} : separators[separator_index(random)];
            }
            markdown += pieces[piece_index(random)];
        }
        if (round % 3 == 0) {
            markdown += "\n";
        }
        INFO("ronda " << round << ":\n" << markdown);
        REQUIRE(check_contract(markdown).empty());
        if (blank_only) {
            for (const SourceBlock& range : source_blocks(markdown)) {
                REQUIRE(range.blocks == 1);
            }
        }
    }
}

TEST_CASE("source_blocks: rendimiento con un documento grande", "[markdown][source_blocks]") {
    std::string markdown;
    for (int i = 0; i < 400; ++i) {
        markdown += "## Sección " + std::to_string(i) +
                    "\n\nUn párrafo de texto normal con algunas palabras.\n\n"
                    "```cpp\nint x = " +
                    std::to_string(i) + ";\n```\n\n- uno\n- dos\n\n";
    }
    const auto start = std::chrono::steady_clock::now();
    const auto ranges = source_blocks(markdown);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    const double ms = std::chrono::duration<double, std::milli>(elapsed).count();
    WARN("source_blocks de " << markdown.size() << " bytes: " << ms << " ms");
    CHECK(ranges.size() == 1600);
    if (chatbot_test::strict_performance()) {
        CHECK(ms < 200.0);
    }
}
