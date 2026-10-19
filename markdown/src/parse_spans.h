#ifndef CHATBOT_MARKDOWN_PARSE_SPANS_H
#define CHATBOT_MARKDOWN_PARSE_SPANS_H

#include "chatbot/markdown/markdown.h"

#include <cstddef>
#include <limits>
#include <string_view>
#include <vector>

namespace chatbot::md::detail {

/// Bytes del texto original que md4c entregó dentro de un bloque de primer
/// nivel: texto, lenguaje de una cerca, tipo de una alerta y marca de una
/// tarea. Las líneas sin nada de eso (cercas sin lenguaje, subrayados de
/// setext, la fila |---| de una tabla, reglas, líneas en blanco) no quedan
/// marcadas. empty(): el bloque no tiene ningún byte marcado (una regla).
struct TextSpan {
    std::size_t begin = std::numeric_limits<std::size_t>::max();
    std::size_t end = 0;

    [[nodiscard]] bool empty() const { return begin >= end; }
};

/// Resultado de parse() y, si md4c parseó el documento, un TextSpan por
/// cada elemento de document.blocks. Si no (texto demasiado grande o un
/// error), spans queda vacío.
struct ParsedSpans {
    Document document;
    std::vector<TextSpan> spans;
};

/// parse() con los rangos de texto de cada bloque de primer nivel. No lanza
/// excepciones.
[[nodiscard]] ParsedSpans parse_with_spans(std::string_view markdown);

} // namespace chatbot::md::detail

#endif // CHATBOT_MARKDOWN_PARSE_SPANS_H
