#ifndef CHATBOT_MARKDOWN_SOURCE_BLOCKS_H
#define CHATBOT_MARKDOWN_SOURCE_BLOCKS_H

#include "chatbot/markdown/markdown.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace chatbot::md {

/// Las líneas de un bloque de código con cerca (``` o ~~~) dentro del texto
/// original, en bytes desde el inicio del documento.
struct CodeFence {
    std::size_t open_begin = 0;    ///< Inicio de la línea de la cerca que abre.
    std::size_t content_begin = 0; ///< Fin de esa línea (después de su '\n').
    /// Inicio de la línea de la cerca que cierra o, si no se cerró, el final
    /// del bloque. El contenido son las líneas de [content_begin, content_end).
    std::size_t content_end = 0;
    /// Fin de la línea de la cerca que cierra (después de su '\n'), o el
    /// final del bloque si no se cerró. Lo que sigue hasta SourceBlock::end
    /// son líneas en blanco, definiciones de enlaces o notas al pie.
    std::size_t close_end = 0;
    std::string fence; ///< Los caracteres de la cerca que abre ("```", "~~~~").
};

/// Uno o más bloques de primer nivel del documento (Document::blocks) y los
/// bytes del texto original que les tocan.
struct SourceBlock {
    Block::Kind kind = Block::Kind::Paragraph; ///< El del primer bloque.
    std::size_t begin = 0; ///< Inicio de una línea.
    std::size_t end = 0;   ///< Inicio de la línea siguiente al bloque, o el tamaño del texto.
    /// Cuántos bloques de Document::blocks hay en [begin, end): casi siempre
    /// 1. Más de 1 si md4c no deja ver dónde termina uno y empieza el
    /// siguiente sin cambiar lo que son: no se puede cortar entre ellos.
    std::size_t blocks = 1;
    /// Solo un bloque Code con cerca (blocks == 1); nullopt en el código con
    /// sangría y en lo demás.
    std::optional<CodeFence> fence;
};

/// Los bloques de primer nivel de parse(markdown) con su rango en bytes, en
/// orden. Los rangos van seguidos y cubren todo el texto: el primero empieza
/// en 0, cada uno termina donde empieza el siguiente y el último termina en
/// markdown.size(). Las líneas en blanco y lo que no es bloque (definiciones
/// de enlaces, notas al pie) quedan con el bloque anterior; las del
/// principio, con el primero. Cada rango, parseado solo, da exactamente sus
/// `blocks` bloques de primer nivel con los mismos tipos que en el
/// documento completo: se revisa volviendo a parsearlo (md4c no da
/// posiciones; se reconstruyen con los punteros de texto que entrega).
/// Vacío si el documento no tiene bloques. No lanza excepciones.
[[nodiscard]] std::vector<SourceBlock> source_blocks(std::string_view markdown);

} // namespace chatbot::md

#endif // CHATBOT_MARKDOWN_SOURCE_BLOCKS_H
