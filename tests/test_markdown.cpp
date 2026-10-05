#include "markdown.h"
#include "performance.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

namespace {

using chatbot::cli::md::Align;
using chatbot::cli::md::Block;
using chatbot::cli::md::Document;
using chatbot::cli::md::Link;
using chatbot::cli::md::parse;
using chatbot::cli::md::Run;
using chatbot::cli::md::sanitize;
namespace md = chatbot::cli::md;

/// Texto plano de los tramos (los saltos duros como '\n').
std::string plain(const std::vector<Run>& runs) {
    std::string text;
    for (const Run& run : runs) {
        text += run.kind == Run::Kind::LineBreak ? std::string{"\n"} : run.text;
    }
    return text;
}

/// Copia del único bloque del documento (copia: el documento suele ser temporal).
Block only_block(const Document& document) {
    REQUIRE(document.blocks.size() == 1);
    return document.blocks.front();
}

/// Copia del primer tramo cuyo texto es exactamente text.
Run run_with(const Block& block, const std::string& text) {
    for (const Run& run : block.runs) {
        if (run.text == text) {
            return run;
        }
    }
    FAIL("no hay un tramo con el texto \"" << text << "\"");
    return block.runs.front();
}

bool has_byte(const std::string& text, char byte) { return text.find(byte) != std::string::npos; }

/// Recorre todo el texto del documento (tramos, código e info).
void collect(const Block& block, std::string& out) {
    out += plain(block.runs) + block.code + block.info;
    for (const Block& child : block.children) {
        collect(child, out);
    }
    for (const auto& row : block.rows) {
        for (const Block& cell : row) {
            collect(cell, out);
        }
    }
}

std::string all_text(const Document& document) {
    std::string out;
    for (const Block& block : document.blocks) {
        collect(block, out);
    }
    for (const Block& block : document.footnotes) {
        collect(block, out);
    }
    return out;
}

} // namespace

TEST_CASE("markdown: texto vacío no produce bloques", "[markdown]") {
    CHECK(parse("").blocks.empty());
    CHECK(parse("   \n\n  ").blocks.empty());
}

TEST_CASE("markdown: párrafo con salto suave y salto duro", "[markdown]") {
    const Document document = parse("una\ndos  \ntres");
    const Block& paragraph = only_block(document);
    CHECK(paragraph.kind == Block::Kind::Paragraph);
    CHECK(plain(paragraph.runs) == "una dos\ntres");
}

TEST_CASE("markdown: encabezados del 1 al 6", "[markdown]") {
    const Document document = parse("# Uno\n## Dos\n### Tres\n#### Cuatro\n##### Cinco\n###### Seis");
    REQUIRE(document.blocks.size() == 6);
    for (unsigned i = 0; i < 6; ++i) {
        CHECK(document.blocks[i].kind == Block::Kind::Heading);
        CHECK(document.blocks[i].level == i + 1);
    }
    CHECK(plain(document.blocks[2].runs) == "Tres");
}

TEST_CASE("markdown: citas anidadas", "[markdown]") {
    const Block& quote = only_block(parse("> afuera\n>\n> > adentro"));
    CHECK(quote.kind == Block::Kind::Quote);
    REQUIRE(quote.children.size() == 2);
    CHECK(plain(quote.children[0].runs) == "afuera");
    CHECK(quote.children[1].kind == Block::Kind::Quote);
    REQUIRE(quote.children[1].children.size() == 1);
    CHECK(plain(quote.children[1].children[0].runs) == "adentro");
}

TEST_CASE("markdown: listas con viñetas anidadas", "[markdown]") {
    const Block& list = only_block(parse("- uno\n  - uno.uno\n- dos"));
    CHECK(list.kind == Block::Kind::BulletList);
    REQUIRE(list.children.size() == 2);
    const Block& first = list.children[0];
    CHECK(first.kind == Block::Kind::ListItem);
    REQUIRE(first.children.size() == 2);
    CHECK(plain(first.children[0].runs) == "uno"); // Párrafo implícito (lista compacta).
    CHECK(first.children[1].kind == Block::Kind::BulletList);
    CHECK(plain(first.children[1].children[0].children[0].runs) == "uno.uno");
}

TEST_CASE("markdown: lista numerada respeta el número inicial", "[markdown]") {
    const Block& list = only_block(parse("3. tres\n4. cuatro"));
    CHECK(list.kind == Block::Kind::OrderedList);
    CHECK(list.start == 3);
    CHECK(list.children.size() == 2);
}

TEST_CASE("markdown: elementos de tarea", "[markdown]") {
    const Block& list = only_block(parse("- [ ] pendiente\n- [x] hecha"));
    REQUIRE(list.children.size() == 2);
    CHECK(list.children[0].task);
    CHECK_FALSE(list.children[0].checked);
    CHECK(list.children[1].task);
    CHECK(list.children[1].checked);
    CHECK(plain(list.children[1].children[0].runs) == "hecha");
}

TEST_CASE("markdown: bloque de código con lenguaje conserva la sangría", "[markdown]") {
    const Block& code = only_block(parse("```cpp\nint main() {\n    return 0;\n}\n```"));
    CHECK(code.kind == Block::Kind::Code);
    CHECK(code.info == "cpp");
    CHECK(code.code == "int main() {\n    return 0;\n}\n");
}

TEST_CASE("markdown: código dentro de una lista y lista dentro de una cita", "[markdown]") {
    const Block& list = only_block(parse("- paso:\n\n  ```sh\n  ls -la\n  ```"));
    REQUIRE(list.children.size() == 1);
    const Block& item = list.children[0];
    REQUIRE(item.children.size() == 2);
    CHECK(item.children[1].kind == Block::Kind::Code);
    CHECK(item.children[1].code == "ls -la\n");

    const Block& quote = only_block(parse("> - a\n> - b"));
    CHECK(quote.kind == Block::Kind::Quote);
    REQUIRE(quote.children.size() == 1);
    CHECK(quote.children[0].kind == Block::Kind::BulletList);
}

TEST_CASE("markdown: regla horizontal", "[markdown]") {
    const Document document = parse("arriba\n\n---\n\nabajo");
    REQUIRE(document.blocks.size() == 3);
    CHECK(document.blocks[1].kind == Block::Kind::Rule);
}

TEST_CASE("markdown: tabla con encabezado y alineación por columna", "[markdown]") {
    const Block& table =
        only_block(parse("| A | B | C | D |\n|---|:--|:-:|--:|\n| 1 | 2 | 3 | 4 |\n| x | y | z | w |"));
    CHECK(table.kind == Block::Kind::Table);
    CHECK(table.header_rows == 1);
    REQUIRE(table.rows.size() == 3);
    REQUIRE(table.align.size() == 4);
    CHECK(table.align[0] == Align::Default);
    CHECK(table.align[1] == Align::Left);
    CHECK(table.align[2] == Align::Center);
    CHECK(table.align[3] == Align::Right);
    CHECK(plain(table.rows[0][2].runs) == "C");
    CHECK(plain(table.rows[2][3].runs) == "w");
}

TEST_CASE("markdown: alertas", "[markdown]") {
    const Block& alert = only_block(parse("> [!WARNING]\n> Cuidado con esto."));
    CHECK(alert.kind == Block::Kind::Alert);
    CHECK(alert.info == "warning");
    REQUIRE_FALSE(alert.children.empty());
    CHECK(plain(alert.children.back().runs) == "Cuidado con esto.");
}

TEST_CASE("markdown: notas al pie (referencia y definición)", "[markdown]") {
    const Document document = parse("Texto[^n].\n\n[^n]: La nota.");
    const Block& paragraph = only_block(document);
    bool found = false;
    for (const Run& run : paragraph.runs) {
        if (run.kind == Run::Kind::FootnoteRef) {
            CHECK(run.footnote == 1);
            found = true;
        }
    }
    CHECK(found);
    CHECK(plain(paragraph.runs).find("^n") == std::string::npos);
    REQUIRE(document.footnotes.size() == 1);
    CHECK(document.footnotes[0].kind == Block::Kind::FootnoteDef);
    CHECK(document.footnotes[0].footnote == 1);
    CHECK(plain(document.footnotes[0].children.at(0).runs) == "La nota.");
}

TEST_CASE("markdown: énfasis, fuerte, tachado y resaltado", "[markdown]") {
    const Block& paragraph = only_block(parse("*em* **fuerte** ~~tachado~~ ==resaltado== ***ambos***"));
    CHECK(run_with(paragraph, "em").style == md::kEmphasis);
    CHECK(run_with(paragraph, "fuerte").style == md::kStrong);
    CHECK(run_with(paragraph, "tachado").style == md::kStrike);
    CHECK(run_with(paragraph, "resaltado").style == md::kHighlight);
    CHECK(run_with(paragraph, "ambos").style == (md::kEmphasis | md::kStrong));
}

TEST_CASE("markdown: guion bajo sigue siendo énfasis (sin MD_FLAG_UNDERLINE)", "[markdown]") {
    const Block& paragraph = only_block(parse("_em_"));
    CHECK(run_with(paragraph, "em").style == md::kEmphasis);
}

TEST_CASE("markdown: código en línea", "[markdown]") {
    const Block& paragraph = only_block(parse("usa `ls -la` aquí"));
    const Run& code = run_with(paragraph, "ls -la");
    CHECK(code.kind == Run::Kind::Code);
}

TEST_CASE("markdown: enlace, imagen y autolink", "[markdown]") {
    const Block& paragraph =
        only_block(parse("[sitio](https://ej.com/a) ![logo](https://ej.com/l.png) https://ej.org"));
    REQUIRE(paragraph.links.size() == 2);
    const Run& link = run_with(paragraph, "sitio");
    REQUIRE(link.link >= 0);
    CHECK(paragraph.links[static_cast<std::size_t>(link.link)].url == "https://ej.com/a");
    CHECK_FALSE(paragraph.links[static_cast<std::size_t>(link.link)].autolink);

    const Run& image = run_with(paragraph, "logo");
    CHECK(image.kind == Run::Kind::Image);
    CHECK(image.url == "https://ej.com/l.png");

    const Run& autolink = run_with(paragraph, "https://ej.org");
    REQUIRE(autolink.link >= 0);
    CHECK(paragraph.links[static_cast<std::size_t>(autolink.link)].autolink);
}

TEST_CASE("markdown: LaTeX en línea y en bloque", "[markdown]") {
    const Block& paragraph = only_block(parse("Sea $x^2$ y $$\\int f$$"));
    CHECK(run_with(paragraph, "x^2").kind == Run::Kind::Math);
    CHECK(run_with(paragraph, "\\int f").kind == Run::Kind::MathDisplay);
}

TEST_CASE("markdown: entidades numéricas y con nombre", "[markdown]") {
    const Block& paragraph = only_block(
        parse("caf&#233; caf&#xE9; &amp; &lt;&gt; &quot;&apos; &copy; &reg; &hellip; &mdash; "
              "&ndash; &laquo;&raquo; &deg; &times; &divide; &euro; &desconocida;"));
    CHECK(plain(paragraph.runs) ==
          "café café & <> \"' © ® … — – «» ° × ÷ € &desconocida;");
    CHECK(md::decode_entity("&nbsp;") == "\xC2\xA0");
    CHECK(md::decode_entity("&#0;") == "\xEF\xBF\xBD");
    CHECK(md::decode_entity("&#x110000;") == "\xEF\xBF\xBD");
}

TEST_CASE("markdown: HTML crudo se muestra como texto", "[markdown]") {
    const Block& paragraph = only_block(parse("hola <b>negrita</b>"));
    CHECK(plain(paragraph.runs) == "hola <b>negrita</b>");
}

TEST_CASE("sanitize: quita controles, ESC, DEL y C1; expande tabuladores", "[markdown][control]") {
    const std::string replacement = "\xEF\xBF\xBD";
    CHECK(sanitize("a\tb") == "a    b");
    CHECK(sanitize("linea\r\nsiguiente") == "linea\nsiguiente");
    CHECK(sanitize("\x1b[31mrojo") == replacement + "[31mrojo");
    CHECK(sanitize("campana\a") == "campana" + replacement);
    CHECK(sanitize("del\x7f") == "del" + replacement);
    CHECK(sanitize("c1:\xC2\x9B" "31m") == "c1:" + replacement + "31m"); // CSI de 8 bits.
    CHECK(sanitize("roto:\xff\xfe") == "roto:" + replacement + replacement);
    CHECK(sanitize("ñandú 🙂 日本") == "ñandú 🙂 日本");
}

TEST_CASE("markdown: las secuencias ESC del modelo no llegan al árbol", "[markdown][control]") {
    const Document document = parse(
        "Hola \x1b]0;titulo\x07 \x1b[2J**fuerte\x1b[31m**\n\n```\ncodigo\x1b[1m\n```\n\n"
        "| a\x1b[5m | b |\n|---|---|\n| \x9b | c |\n\n[x](https://ej.com/\x1b)");
    const std::string text = all_text(document);
    CHECK_FALSE(has_byte(text, '\x1b'));
    CHECK_FALSE(has_byte(text, '\x07'));
    CHECK(text.find("\xC2\x9B") == std::string::npos);
    for (const Block& block : document.blocks) {
        for (const Link& link : block.links) {
            CHECK_FALSE(has_byte(link.url, '\x1b'));
        }
    }
}

TEST_CASE("markdown: markdown incompleto nunca falla", "[markdown]") {
    // Fence sin cerrar: todo lo que sigue es código.
    const Block& code = only_block(parse("```python\nprint('hola')\nx = 1"));
    CHECK(code.kind == Block::Kind::Code);
    CHECK(code.code.find("x = 1") != std::string::npos);

    // ** sin cerrar: queda como texto literal.
    const Block& paragraph = only_block(parse("esto es **fuerte sin cerrar"));
    CHECK(plain(paragraph.runs) == "esto es **fuerte sin cerrar");

    // Tabla a medias y lista cortada.
    CHECK_FALSE(parse("| A | B |\n|---|").blocks.empty());
    CHECK_FALSE(parse("| A | B |\n|---|---|\n| 1 |").blocks.empty());
    CHECK_FALSE(parse("1. uno\n2.").blocks.empty());
    CHECK_FALSE(parse("> [!NOTE").blocks.empty());
    CHECK_FALSE(parse("[enlace](https://sin-cerrar").blocks.empty());
}

TEST_CASE("markdown: documento de 200 KB", "[markdown][desempeno]") {
    std::string big;
    while (big.size() < 200 * 1024) {
        big += "## Sección\n\nTexto con **negritas**, *cursivas*, `código` y un "
               "[enlace](https://ej.com). Más texto para llenar la línea.\n\n"
               "- uno\n- dos\n  - dos.uno\n\n"
               "| A | B |\n|:--|--:|\n| 1 | 2 |\n\n"
               "```cpp\nint x = 0;\n```\n\n> cita\n\n";
    }
    // Se mide tres veces y cuenta la más rápida: así una pausa de la máquina
    // (otros procesos, un runner de CI compartido) no hace fallar la prueba,
    // y un parser lento sigue fallando en las tres.
    auto elapsed = std::chrono::microseconds::max();
    for (int i = 0; i < 3; ++i) {
        const auto start = std::chrono::steady_clock::now();
        const Document document = parse(big);
        elapsed = std::min(elapsed, std::chrono::duration_cast<std::chrono::microseconds>(
                                        std::chrono::steady_clock::now() - start));
        CHECK(document.blocks.size() > 1000);
    }
    const double ms = std::chrono::duration<double, std::milli>(elapsed).count();
    WARN("parseo de " << big.size() / 1024 << " KB: " << ms << " ms");
    if (chatbot_test::strict_performance()) {
        CHECK(elapsed < std::chrono::milliseconds{100});
    }
}
