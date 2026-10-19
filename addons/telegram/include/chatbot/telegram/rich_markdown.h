#ifndef CHATBOT_TELEGRAM_RICH_MARKDOWN_H
#define CHATBOT_TELEGRAM_RICH_MARKDOWN_H

#include "chatbot/bot/types.h"

#include <cstddef>
#include <string>
#include <string_view>

namespace chatbot::telegram {

/// Topes de un mensaje enriquecido (sendRichMessage, Bot API 10.1): 32768
/// "UTF-8 characters" del texto y 500 bloques. La documentación no dice si
/// cuenta caracteres o bytes: se miden bytes del markdown ya saneado, que
/// nunca son menos.
inline constexpr std::size_t kRichMaxBytes = 32768;
inline constexpr std::size_t kRichMaxBlocks = 500;
/// Tope de sendMessage: 4096 caracteres (unidades UTF-16).
inline constexpr std::size_t kPlainMaxUnits = 4096;

/// Convierte el markdown del modelo (texto no confiable: puede venir de
/// páginas de /buscar) en el markdown enriquecido que se manda a Telegram.
/// Se rearma desde el árbol de md::parse con una lista blanca: párrafos,
/// encabezados, listas (con tareas), citas, alertas (como cita con título),
/// tablas, reglas, código, fórmulas, notas al pie, negritas, cursivas,
/// tachado, resaltado y enlaces. Así:
/// - ninguna etiqueta HTML llega como etiqueta: el texto va escapado (también
///   lo que Telegram entiende aparte: ||spoiler||, ==marcado==, $fórmula$);
/// - una imagen o un medio se vuelve un enlace normal (con la URL de texto
///   si no tiene texto alternativo), nunca un bloque de medios;
/// - solo los enlaces http, https y mailto quedan como enlaces; los demás
///   (tg://, javascript:, tel:…) quedan como texto;
/// - el contenido de los bloques y fragmentos de código no cambia (en una
///   fórmula, < y > pasan a \lt y \gt).
/// Función pura: no lanza excepciones.
[[nodiscard]] std::string to_rich_markdown(std::string_view markdown);

/// Bloques del mensaje enriquecido que sale de to_rich_markdown, contados
/// como Telegram (anidados, elementos de lista, filas de tabla, citas) y
/// de más cuando no se sabe (un párrafo dentro de un elemento de lista
/// cuenta aparte, y cada fórmula en bloque también).
[[nodiscard]] std::size_t rich_block_count(std::string_view markdown);

/// true si la URL puede quedar como enlace: http:// o https:// con algo
/// después (is_web_url) o mailto: con una dirección.
[[nodiscard]] bool is_allowed_link(std::string_view url);

/// Tope de un mensaje enriquecido para chatbot::bot: bytes de
/// to_rich_markdown y rich_block_count.
[[nodiscard]] bot::MessageLimit rich_message_limit();

/// Tope de sendMessage (texto plano): kPlainMaxUnits unidades UTF-16.
[[nodiscard]] bot::MessageLimit plain_message_limit();

} // namespace chatbot::telegram

#endif // CHATBOT_TELEGRAM_RICH_MARKDOWN_H
