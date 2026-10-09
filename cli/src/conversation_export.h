#ifndef CHATBOT_CLI_CONVERSATION_EXPORT_H
#define CHATBOT_CLI_CONVERSATION_EXPORT_H

#include "conversation_store.h"

#include <ctime>
#include <string>
#include <string_view>

namespace chatbot::cli {

/// Markdown de la conversación para /exportar (función pura):
/// - "# <título>" ("Conversación" si no tiene);
/// - una línea con la fecha de creación ("AAAA-MM-DD HH:MM", o la de
///   fallback_date si no tiene) y los modelos de las respuestas;
/// - por cada par, "## Tú" con el texto tal como se escribió (el "/buscar
///   …", no el bloque de resultados), "## Asistente" con la respuesta y,
///   si hubo búsqueda, "### Fuentes" como lista numerada:
///   "1. [título](url) — AAAA-MM-DD" (sin fecha si no hay). Una URL que no
///   pasa is_web_url va como texto plano, sin enlace.
/// Nunca incluye las instrucciones de sistema, los bloques de resultados,
/// keys, avisos ni errores: solo los pares guardados.
[[nodiscard]] std::string export_markdown(const StoredConversation& conversation,
                                          std::string_view fallback_date);

/// "AAAA-MM-DD HH:MM" a partir de una fecha ISO 8601 ("2026-10-02T23:58:00-06:00");
/// si no tiene esa forma, la devuelve tal cual.
[[nodiscard]] std::string export_date(std::string_view iso8601);

/// "conversacion-<id>.md", o "conversacion-AAAAMMDD-HHMMSS.md" (hora local
/// de now) si la conversación no tiene id.
[[nodiscard]] std::string export_file_name(std::string_view id, std::time_t now);

} // namespace chatbot::cli

#endif // CHATBOT_CLI_CONVERSATION_EXPORT_H
