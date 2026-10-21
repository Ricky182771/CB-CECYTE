#ifndef CHATBOT_TELEGRAM_INTEGER_H
#define CHATBOT_TELEGRAM_INTEGER_H

// Interno del addon (no es un header público): las pruebas lo incluyen
// desde src/, como las del núcleo con core/src.

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string_view>

namespace chatbot::telegram {

/// Cadena de dígitos decimales, con '-' solo al principio, que cabe en
/// int64_t; nullopt si no (vacía, espacios, '+', "1.0", "1e3", "0x10",
/// fuera de rango o con algo más al final). Con std::from_chars, que no
/// acepta espacios ni '+', y exigiendo que consuma toda la cadena.
[[nodiscard]] std::optional<std::int64_t> parse_decimal_int64(std::string_view text);

/// Un entero de un update de la Bot API: número entero de JSON (que quepa en
/// int64_t) o cadena que acepte parse_decimal_int64. Telegram manda el
/// draft_id de stopped_message_generation como cadena aunque la
/// documentación diga Integer (confirmado el 2026-10-20): todos los enteros
/// de los updates pasan por aquí. nullopt con null, booleano, decimal,
/// objeto o arreglo.
[[nodiscard]] std::optional<std::int64_t> telegram_integer(const nlohmann::json& value);

} // namespace chatbot::telegram

#endif // CHATBOT_TELEGRAM_INTEGER_H
