#ifndef CHATBOT_PUBLISHED_DATE_H
#define CHATBOT_PUBLISHED_DATE_H

#include <string>
#include <string_view>

namespace chatbot {

/// Deja la fecha de publicación de un resultado como "AAAA-MM-DD" si tiene
/// una de estas formas (función pura):
/// - ISO 8601: "2026-10-04", o seguida de 'T' o de un espacio y la hora
///   ("2026-10-04T17:00:00Z"); se toma la fecha tal cual, sin convertir zona.
/// - RFC 1123: "Sun, 04 Oct 2026 17:00:00 GMT" (el día de la semana y la
///   hora son opcionales; el mes, en inglés, sin distinguir mayúsculas).
/// La fecha debe existir (mes 1–12, día válido para ese mes y año). Si no se
/// reconoce, la devuelve tal cual; vacía, vacía.
[[nodiscard]] std::string normalize_published_date(std::string_view date);

} // namespace chatbot

#endif // CHATBOT_PUBLISHED_DATE_H
