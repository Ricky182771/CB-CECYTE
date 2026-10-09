#ifndef CHATBOT_UTF8_H
#define CHATBOT_UTF8_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace chatbot::utf8 {

// Decodificador UTF-8 mínimo: lo usan el núcleo (limpieza de los resultados
// de búsqueda) y cli/ (validación de las instrucciones del sistema).

/// U+FFFD en UTF-8: reemplaza las secuencias inválidas.
inline constexpr std::string_view kReplacement = "\xEF\xBF\xBD";

/// Decodifica el código en text[i] y avanza i al siguiente. Nullopt, sin
/// mover i, si la secuencia no es UTF-8 válido (incompleta, sobrelarga,
/// sustituto o mayor que U+10FFFF). Requiere i < text.size().
[[nodiscard]] std::optional<std::uint32_t> next_code_point(std::string_view text,
                                                           std::size_t& i);

/// true si todo el texto es UTF-8 válido.
[[nodiscard]] bool is_valid(std::string_view text);

} // namespace chatbot::utf8

#endif // CHATBOT_UTF8_H
