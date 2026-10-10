#ifndef CHATBOT_SRC_REDIRECT_H
#define CHATBOT_SRC_REDIRECT_H

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace chatbot {

// Respuestas 3xx: el transporte no sigue redirecciones; estas funciones
// explican a dónde redirige el servidor. La URL viene del servidor: es un
// dato no confiable. Header interno (src/); no forma parte de la API pública.

/// Bytes máximos de redirect_display, contando el "…" final.
inline constexpr std::size_t kMaxRedirectDisplayBytes = 200;

/// URL de la redirección lista para mostrarse: sin query ni fragmento (pueden
/// traer la key), sin usuario ni contraseña ("https://u:p@h/x" → "https://h/x"),
/// sin controles (C0, DEL y C1), con U+FFFD en lugar de los bytes UTF-8
/// inválidos y recortada a kMaxRedirectDisplayBytes sin partir un carácter,
/// terminando en "…" si se recortó.
[[nodiscard]] std::string redirect_display(std::string_view redirect_url);

/// Base sugerida: si la redirección (sin query ni fragmento) termina en path
/// ("/models" o "/chat/completions"), lo que queda antes, sin "/" final. Solo
/// si pasa validate_base_url, se muestra igual que la envía el servidor
/// (redirect_display no le cambia nada) y es distinta de la base actual (la
/// de request_url sin path); si no, nullopt.
[[nodiscard]] std::optional<std::string> suggest_base_url(std::string_view request_url,
                                                          std::string_view redirect_url,
                                                          std::string_view path);

} // namespace chatbot

#endif // CHATBOT_SRC_REDIRECT_H
