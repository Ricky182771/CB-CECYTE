#ifndef CHATBOT_BOT_SPLIT_H
#define CHATBOT_BOT_SPLIT_H

#include "chatbot/bot/types.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace chatbot::bot {

/// Tope mínimo que aceptan las funciones de partir: uno menor se trata como
/// este (siempre debe caber cualquier carácter y una cerca corta).
inline constexpr std::size_t kMinMessageLimit = 16;

/// Parte la respuesta del modelo (markdown) en pedazos que caben en
/// limit.max_size según limit.measure. Corta entre bloques de primer nivel
/// (md::source_blocks), juntando los que quepan. Un bloque que no cabe solo:
/// - código con cerca: se parte por líneas en varios bloques, cada uno con
///   la línea de la cerca original (y su lenguaje) y una cerca que cierra;
/// - lo demás (párrafos, listas, tablas…): por líneas, luego en un espacio
///   y, si no hay, entre caracteres.
/// Ningún pedazo pasa del tope ni queda vacío (ni solo con espacios); a cada
/// uno se le quitan las líneas en blanco del principio y los espacios del
/// final. Juntos tienen el mismo contenido que el original, salvo los
/// espacios donde se cortó y las cercas que se repiten.
[[nodiscard]] std::vector<std::string> split_markdown(std::string_view markdown,
                                                      const MessageLimit& limit);

/// Igual, para texto sin formato (avisos, fuentes): por líneas, luego en un
/// espacio y, si no hay, entre caracteres.
[[nodiscard]] std::vector<std::string> split_plain(std::string_view text,
                                                   const MessageLimit& limit);

} // namespace chatbot::bot

#endif // CHATBOT_BOT_SPLIT_H
