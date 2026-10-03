#ifndef CHATBOT_CLI_MARKDOWN_H
#define CHATBOT_CLI_MARKDOWN_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace chatbot::cli::md {

/// Estilos en línea; se combinan como bits.
enum Style : std::uint8_t {
    kPlain = 0,
    kEmphasis = 1U << 0U,
    kStrong = 1U << 1U,
    kStrike = 1U << 2U,
    kHighlight = 1U << 3U,
};

/// Un tramo de contenido en línea con estilo uniforme.
struct Run {
    enum class Kind {
        Text,        ///< Texto normal (con estilos).
        Code,        ///< Código en línea.
        Math,        ///< LaTeX en línea (texto crudo).
        MathDisplay, ///< LaTeX en bloque, $$...$$ (texto crudo).
        LineBreak,   ///< Salto duro.
        Image,       ///< text = alt, url = dirección.
        FootnoteRef, ///< footnote = número de la nota.
    };
    Kind kind = Kind::Text;
    std::string text;
    std::uint8_t style = kPlain;
    int link = -1;          ///< Índice en Block::links si está dentro de un enlace.
    std::string url;        ///< Solo Image.
    unsigned footnote = 0;  ///< Solo FootnoteRef.
};

/// Un enlace del bloque; los tramos que lo forman apuntan a él.
struct Link {
    std::string url;
    bool autolink = false;
};

enum class Align { Default, Left, Center, Right };

/// Un bloque del documento.
struct Block {
    enum class Kind {
        Document,
        Paragraph,
        Heading,     ///< level = 1..6
        Quote,
        Alert,       ///< info = note/tip/important/warning/caution
        BulletList,
        OrderedList, ///< start = número inicial
        ListItem,    ///< task/checked
        Code,        ///< info = lenguaje, code = texto
        Rule,
        Table,
        FootnoteDef, ///< footnote = número de la nota
    };
    Kind kind = Kind::Paragraph;

    std::vector<Run> runs;   ///< Paragraph y Heading (y celdas de tabla).
    std::vector<Link> links;
    std::vector<Block> children;

    unsigned level = 0;
    unsigned start = 1;
    bool task = false;
    bool checked = false;
    std::string info;
    std::string code;
    unsigned footnote = 0;

    // Tabla: cada fila es una lista de celdas (bloques Paragraph).
    std::vector<Align> align;
    std::vector<std::vector<Block>> rows;
    std::size_t header_rows = 0;
};

/// Documento parseado: bloques de contenido y definiciones de notas al pie.
struct Document {
    std::vector<Block> blocks;
    std::vector<Block> footnotes; ///< Bloques FootnoteDef, en orden.
};

/// Convierte markdown en el árbol propio con md4c (MD_DIALECT_GITHUB |
/// MD_FLAG_LATEXMATHSPANS | MD_FLAG_HIGHLIGHT | MD_FLAG_NOHTML). Nunca
/// falla: el markdown incompleto se parsea como md4c lo entienda y, si md4c
/// no pudiera, el texto queda como un solo párrafo. No lanza excepciones.
[[nodiscard]] Document parse(std::string_view markdown);

/// Quita lo que podría controlar la terminal: los controles C0 (salvo '\n')
/// y C1, ESC y DEL se reemplazan por U+FFFD, '\r' se quita, '\t' pasa a 4
/// espacios y los bytes que no son UTF-8 válido se reemplazan por U+FFFD.
[[nodiscard]] std::string sanitize(std::string_view text);

/// Decodifica una entidad HTML ("&amp;", "&#233;", "&#xE9;"); una entidad
/// desconocida se devuelve tal cual.
[[nodiscard]] std::string decode_entity(std::string_view entity);

} // namespace chatbot::cli::md

#endif // CHATBOT_CLI_MARKDOWN_H
