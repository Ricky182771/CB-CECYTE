// Saneado del markdown del modelo antes de mandarlo como mensaje enriquecido
// (to_rich_markdown) y cómo se cuentan sus bloques.

#include "chatbot/telegram/rich_markdown.h"

#include "chatbot/markdown/markdown.h"

#include <catch2/catch_test_macros.hpp>

#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace {

using chatbot::telegram::is_allowed_link;
using chatbot::telegram::rich_block_count;
using chatbot::telegram::to_rich_markdown;
namespace md = chatbot::md;

bool contains(std::string_view text, std::string_view part) {
    return text.find(part) != std::string_view::npos;
}

/// md::parse como lo hace el saneado (código crudo).
md::Document parse_raw(std::string_view markdown) {
    md::ParseOptions options;
    options.raw_code = true;
    options.keep_soft_breaks = true;
    return md::parse(markdown, options);
}

/// Todo el código (bloques y fragmentos) en el orden del documento.
void collect_code(const std::vector<md::Block>& blocks, std::vector<std::string>& out) {
    for (const md::Block& block : blocks) {
        if (block.kind == md::Block::Kind::Code) {
            out.push_back(block.code);
        }
        for (const md::Run& run : block.runs) {
            if (run.kind == md::Run::Kind::Code) {
                out.push_back(run.text);
            }
        }
        collect_code(block.children, out);
        for (const std::vector<md::Block>& row : block.rows) {
            collect_code(row, out);
        }
    }
}

std::vector<std::string> code_of(std::string_view markdown) {
    const md::Document document = parse_raw(markdown);
    std::vector<std::string> out;
    collect_code(document.blocks, out);
    collect_code(document.footnotes, out);
    return out;
}

/// Imágenes y enlaces del documento ya saneado, para revisar que no quede
/// ningún medio ni esquema prohibido.
void collect_links(const std::vector<md::Block>& blocks, std::vector<std::string>& links,
                   std::size_t& images) {
    for (const md::Block& block : blocks) {
        for (const md::Link& link : block.links) {
            links.push_back(link.url);
        }
        for (const md::Run& run : block.runs) {
            images += run.kind == md::Run::Kind::Image ? 1 : 0;
        }
        collect_links(block.children, links, images);
        for (const std::vector<md::Block>& row : block.rows) {
            collect_links(row, links, images);
        }
    }
}

/// Fuera del código, el texto saneado no tiene ningún '<' sin escapar (una
/// etiqueta): se revisa reparseando y buscando '<' en el texto de los
/// tramos, que md4c entrega ya sin las barras de escape, contra el
/// markdown: cada '<' del markdown fuera del código lleva su '\'.
bool has_raw_tag(std::string_view rich) {
    // Quita el código (lo que md4c dice que es código) y revisa el resto.
    std::string outside;
    std::size_t i = 0;
    while (i < rich.size()) {
        const char c = rich[i];
        if (c == '\\' && i + 1 < rich.size()) {
            i += 2; // Escapado.
            continue;
        }
        if (c == '`') {
            std::size_t ticks = 0;
            while (i + ticks < rich.size() && rich[i + ticks] == '`') {
                ++ticks;
            }
            const std::string fence(ticks, '`');
            const std::size_t close = rich.find(fence, i + ticks);
            i = close == std::string_view::npos ? rich.size() : close + ticks;
            continue;
        }
        if (c == '<') {
            return true;
        }
        ++i;
    }
    return false;
}

/// true si fuera del código hay un "\\<" o un "\\&": Telegram deja ver esa
/// barra (lo confirmó la sonda); '<' y '&' van como entidades.
bool has_backslash_entity(std::string_view rich) {
    std::size_t i = 0;
    while (i < rich.size()) {
        const char c = rich[i];
        if (c == '`') {
            std::size_t ticks = 0;
            while (i + ticks < rich.size() && rich[i + ticks] == '`') {
                ++ticks;
            }
            const std::string fence(ticks, '`');
            const std::size_t close = rich.find(fence, i + ticks);
            i = close == std::string_view::npos ? rich.size() : close + ticks;
            continue;
        }
        if (c == '\\' && i + 1 < rich.size()) {
            if (rich[i + 1] == '<' || rich[i + 1] == '&') {
                return true;
            }
            i += 2;
            continue;
        }
        ++i;
    }
    return false;
}

} // namespace

TEST_CASE("saneado: < y & van como entidades, no con barra", "[telegram][saneado]") {
    CHECK(to_rich_markdown("a <b>c</b> & d") == "a &lt;b\\>c&lt;/b\\> &amp; d");
    // Una entidad que escribió el modelo se ve tal cual (no se decodifica
    // dos veces): md4c la entiende como "<" y sale como &lt;.
    CHECK(to_rich_markdown("&lt;b&gt;") == "&lt;b\\>");
    CHECK(to_rich_markdown("AT&amp;T y &amp;lt;") == "AT&amp;T y &amp;lt;");
    CHECK(to_rich_markdown("Tom & Jerry") == "Tom &amp; Jerry");
    // Al principio de una línea, '<' tampoco abre un bloque HTML.
    CHECK(to_rich_markdown("<div>\nhola\n</div>") == "&lt;div\\>\nhola\n&lt;/div\\>");
    // El código no cambia.
    CHECK(to_rich_markdown("`a<b && c`") == "`a<b && c`");
    // md4c (y Telegram) leen el mismo texto.
    const md::Document document = md::parse(to_rich_markdown("x < y & z"));
    REQUIRE(document.blocks.size() == 1);
    CHECK(document.blocks[0].runs[0].text == "x < y & z");
}

TEST_CASE("saneado: el HTML del modelo va como texto", "[telegram][saneado]") {
    for (const char* tag :
         {"<tg-button type=\"url\" url=\"https://malo.example\">Premio</tg-button>",
          "<img src=\"https://malo.example/x.png\"/>", "<tg-map lat=\"19.4\" long=\"-99.1\"/>",
          "<details open><summary>Abre</summary>secreto</details>", "<b>negritas</b>",
          "<tg-thinking>falso</tg-thinking>", "<a href=\"tg://user?id=1\">yo</a>",
          "<video src=\"https://x.example/v.mp4\"></video>", "&lt;b&gt;entidad&lt;/b&gt;"}) {
        const std::string rich = to_rich_markdown(std::string{"Texto "} + tag + " fin.");
        INFO(rich);
        CHECK_FALSE(has_raw_tag(rich));
        CHECK(contains(rich, "Texto "));
        CHECK(contains(rich, "fin."));
    }
    // Un bloque HTML al principio de una línea también.
    const std::string block = to_rich_markdown("<details>\n<summary>x</summary>\n\nhola\n</details>");
    INFO(block);
    CHECK_FALSE(has_raw_tag(block));
    // Lo que se lee no cambia: md4c ve el mismo texto.
    const md::Document document = md::parse(to_rich_markdown("a <b>c</b> d"));
    REQUIRE(document.blocks.size() == 1);
    REQUIRE(document.blocks[0].runs.size() == 1);
    CHECK(document.blocks[0].runs[0].text == "a <b>c</b> d");
}

TEST_CASE("saneado: el código no se toca", "[telegram][saneado]") {
    const std::string source = "Usa `<tg-button>` así:\n\n"
                               "```html\n<tg-button type=\"url\" url=\"https://x\">x</tg-button>\n"
                               "\t<img src=x> $5 **no** [a](tg://x)\n```\n";
    const std::string rich = to_rich_markdown(source);
    INFO(rich);
    CHECK(contains(rich, "`<tg-button>`"));
    CHECK(contains(rich, "```html\n<tg-button type=\"url\" url=\"https://x\">x</tg-button>\n"
                         "\t<img src=x> $5 **no** [a](tg://x)\n```"));
    CHECK(code_of(rich) == code_of(source));
    // md4c entrega la sangría como espacios; el saneado toma el bloque del
    // texto original, así que el tabulador se queda.
    CHECK(contains(rich, "\t<img src=x>"));
    // Un bloque con backticks adentro lleva una cerca más larga.
    const std::string fenced = to_rich_markdown("````\n```\nadentro\n```\n````\n");
    CHECK(code_of(fenced) == code_of("````\n```\nadentro\n```\n````\n"));
    // Un bloque sin cerrar (vista previa a medias) sale cerrado.
    const std::string open = to_rich_markdown("```python\nprint('<b>')\n");
    CHECK(open == "```python\nprint('<b>')\n```");
    // Código con sangría: queda como bloque con cerca, igual por dentro.
    CHECK(code_of(to_rich_markdown("    <x>\n    y\n")) == code_of("    <x>\n    y\n"));
}

TEST_CASE("saneado: las imágenes y los medios quedan como enlaces", "[telegram][saneado]") {
    const std::string rich = to_rich_markdown(
        "Mira: ![un gato](https://example.com/gato.jpg)\n\n"
        "![](https://example.com/video.mp4 \"Video\")\n\n"
        "![sin esquema](tg://photo?id=123)\n\n![emoji](tg://emoji?id=5368324170671202286)");
    INFO(rich);
    CHECK(contains(rich, "[un gato](https://example.com/gato.jpg)"));
    CHECK(contains(rich, "](https://example.com/video.mp4)"));
    CHECK_FALSE(contains(rich, "!["));
    CHECK_FALSE(contains(rich, "tg://"));
    CHECK(contains(rich, "sin esquema"));
    std::vector<std::string> links;
    std::size_t images = 0;
    collect_links(md::parse(rich).blocks, links, images);
    CHECK(images == 0);
    CHECK(links == std::vector<std::string>{"https://example.com/gato.jpg",
                                            "https://example.com/video.mp4"});
}

TEST_CASE("saneado: solo http, https y mailto quedan como enlaces", "[telegram][saneado]") {
    struct Case {
        const char* url;
        bool allowed;
    };
    for (const Case& c : {Case{"https://sep.gob.mx", true}, Case{"http://localhost:8080/x", true},
                          Case{"HTTPS://EJEMPLO.MX", true}, Case{"mailto:profe@escuela.mx", true},
                          Case{"MailTo:a@b.mx", true}, Case{"tg://resolve?domain=x", false},
                          Case{"javascript:alert(1)", false}, Case{"tel:+525512345678", false},
                          Case{"file:///etc/passwd", false}, Case{"data:text/html,x", false},
                          Case{"ftp://x.example", false}, Case{"https://", false},
                          Case{"mailto:", false}, Case{"#ancla", false}, Case{"/ruta", false}}) {
        INFO(c.url);
        CHECK(is_allowed_link(c.url) == c.allowed);
        const std::string rich = to_rich_markdown(std::string{"[clic aquí]("} + c.url + ")");
        std::vector<std::string> links;
        std::size_t images = 0;
        collect_links(md::parse(rich).blocks, links, images);
        if (c.allowed) {
            CHECK(links.size() == 1);
        } else {
            CHECK(links.empty());
            CHECK(contains(rich, "clic aquí"));
        }
    }
    // Un enlace de referencia con un esquema prohibido tampoco.
    const std::string reference = to_rich_markdown("[aquí][1]\n\n[1]: tg://user?id=1");
    CHECK_FALSE(contains(reference, "tg://"));
    // Los autoenlaces quedan como enlaces explícitos.
    const std::string bare = to_rich_markdown("Ve a https://sep.gob.mx o escribe a a@b.mx");
    CHECK(contains(bare, "](https://sep.gob.mx)"));
    CHECK(contains(bare, "](mailto:a@b.mx)"));
    // Una URL con paréntesis o espacios no rompe el destino.
    const std::string odd = to_rich_markdown("[x](<https://e.mx/a b(c)>)");
    CHECK(contains(odd, "(https://e.mx/a%20b%28c%29)"));
}

TEST_CASE("saneado: lo que Telegram entiende aparte va escapado", "[telegram][saneado]") {
    const std::string rich =
        to_rich_markdown("||spoiler|| ==marcado== Cuesta $50 y la otra $60. a=b, x==y");
    INFO(rich);
    CHECK(contains(rich, "\\|\\|spoiler\\|\\|"));
    CHECK(contains(rich, "==marcado==")); // md4c lo ve como resaltado: se queda.
    CHECK(contains(rich, "\\$50"));
    CHECK(contains(rich, "\\$60"));
    CHECK(contains(rich, "a=b")); // Un '=' solo no hace falta.
    // Una fórmula que md4c (y la app) ven como fórmula se queda; < y > con \lt y \gt.
    const std::string math = to_rich_markdown("Si $x^2 + y^2$ y $a<b$ entonces $$E = mc^2$$");
    INFO(math);
    CHECK(contains(math, "$x^2 + y^2$"));
    CHECK(contains(math, "$a\\lt b$"));
    CHECK(contains(math, "$$E = mc^2$$"));
}

TEST_CASE("saneado: el formato del modelo se conserva", "[telegram][saneado]") {
    const std::string source = "# Título\n\n"
                               "Texto con **negritas**, *cursiva*, ~~tachado~~ y `código`.\n"
                               "Segunda línea del párrafo.\n\n"
                               "- uno\n- dos\n  - anidado\n\n"
                               "1. primero\n2. segundo\n\n"
                               "- [ ] pendiente\n- [x] hecho\n\n"
                               "> cita\n\n"
                               "| a | b |\n|:--|--:|\n| 1 | 2 |\n\n"
                               "Nota[^n].\n\n[^n]: La nota.\n\n---\n\nFin.";
    const std::string rich = to_rich_markdown(source);
    INFO(rich);
    for (const char* part :
         {"# Título", "**negritas**", "*cursiva*", "~~tachado~~", "`código`",
          "`código`.\nSegunda línea", "- uno\n- dos\n  - anidado", "1. primero\n2. segundo",
          "- [ ] pendiente", "- [x] hecho", "> cita", "| a | b |\n|:---|---:|\n| 1 | 2 |",
          "Nota[^1]", "[^1]: La nota.", "***", "Fin."}) {
        INFO(part);
        CHECK(contains(rich, part));
    }
    // Las alertas quedan como cita con título.
    CHECK(to_rich_markdown("> [!WARNING]\n> Cuidado") == "> **Advertencia**\n>\n> Cuidado");
    // Lo que abriría un bloque al principio de una línea va escapado.
    CHECK(to_rich_markdown("1\\. no es lista\n\\# ni título\n\\- ni viñeta") ==
          "1\\. no es lista\n\\# ni título\n\\- ni viñeta");
}

TEST_CASE("saneado: sanear dos veces da lo mismo", "[telegram][saneado]") {
    std::vector<std::string> samples = {
        "Texto <b>raro</b> con $5 y **negritas *anidadas* aquí** y `x`.",
        "- uno\n- dos\n\n  párrafo del dos\n\n```c\nint x;\n```",
        "> cita con [enlace](https://a.mx) y ![img](https://a.mx/i.png)\n> más",
        "| a | b |\n|---|---|\n| <x> | `|` |",
        "1. uno\n   ```\n   código\n   ```\n2. dos",
        "Línea con\\\nsalto duro y ||spoiler|| ==m== ~~t~~",
        "[^a]: nota con **formato**\n\nTexto[^a]"};
    const std::size_t kCurated = samples.size();
    // Además, mezclas al azar de pedazos con caracteres especiales.
    const std::vector<std::string> parts = {
        "<", ">", "&", "$", "`", "*", "_", "~", "|", "=", "[", "]", "(", ")", "!", "#", "-",
        "1.", "\n", "\n\n", " ", "texto", "**", "```", "> ", "- ", "\\", "https://a.mx", "ñ",
        "![x](https://b.mx)", "[y](tg://z)", "$a$", "<tg-button>"};
    std::mt19937 random{2026};
    for (int i = 0; i < 400; ++i) {
        std::string sample;
        const std::size_t count = 1 + random() % 25;
        for (std::size_t k = 0; k < count; ++k) {
            sample += parts[random() % parts.size()];
        }
        samples.push_back(sample);
    }
    for (std::size_t s = 0; s < samples.size(); ++s) {
        const std::string& sample = samples[s];
        const std::string once = to_rich_markdown(sample);
        INFO(sample);
        INFO(once);
        if (s < kCurated) {
            // Las muestras normales salen iguales en la segunda pasada. Las de
            // al azar no siempre: una marca de estilo pegada a puntuación
            // puede no reconocerse otra vez (se ve el '*'); eso no es un
            // riesgo, y lo de abajo se revisa en todas.
            CHECK(to_rich_markdown(once) == once);
        }
        const std::string twice = to_rich_markdown(once);
        CHECK_FALSE(has_raw_tag(twice));
        CHECK_FALSE(has_backslash_entity(once));
        CHECK_FALSE(has_raw_tag(once));
        // El código no cambia nunca.
        CHECK(code_of(once) == code_of(sample));
        std::vector<std::string> links;
        std::size_t images = 0;
        collect_links(md::parse(once).blocks, links, images);
        CHECK(images == 0);
        for (const std::string& link : links) {
            CHECK(is_allowed_link(link));
        }
    }
}

TEST_CASE("bloques: se cuentan anidados, elementos y filas", "[telegram][bloques]") {
    CHECK(rich_block_count("") == 0);
    CHECK(rich_block_count("Un párrafo.") == 1);
    CHECK(rich_block_count("# T\n\nP") == 2);
    // Lista + 3 elementos (+ el párrafo de cada uno, de más).
    CHECK(rich_block_count("- a\n- b\n- c") >= 4);
    // Tabla + 3 filas.
    CHECK(rich_block_count("| a |\n|---|\n| 1 |\n| 2 |") >= 4);
    // Cita + párrafo adentro.
    CHECK(rich_block_count("> hola") >= 2);
    // 600 elementos de lista no caben en un mensaje.
    std::string list;
    for (int i = 0; i < 600; ++i) {
        list += "- elemento\n";
    }
    CHECK(rich_block_count(list) > chatbot::telegram::kRichMaxBlocks);
    const chatbot::bot::MessageLimit limit = chatbot::telegram::rich_message_limit();
    CHECK(limit.max_blocks == chatbot::telegram::kRichMaxBlocks);
    CHECK(limit.blocks_of(list) == rich_block_count(list));
    // El tamaño se mide sobre lo saneado (que crece con los escapes).
    CHECK(limit.size_of("a<b") == to_rich_markdown("a<b").size());
    CHECK(limit.size_of("a<b") > std::string_view{"a<b"}.size());
}
