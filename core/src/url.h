#ifndef CHATBOT_URL_H
#define CHATBOT_URL_H

#include <optional>
#include <string>
#include <string_view>

namespace chatbot {

/// Partes de una URL base, ya analizadas. Header interno (src/).
struct ParsedUrl {
    std::string scheme; ///< En minúsculas: "http", "https"...
    std::string host;   ///< En minúsculas; una IPv6 va con sus corchetes ("[::1]").
    int port = 0;       ///< 0 si la URL no trae puerto.
};

/// Analiza scheme://host[:puerto][/ruta]. nullopt si no tiene esa forma: sin
/// "://", sin host, con usuario o contraseña ("usuario@host"), con un puerto
/// que no es un número entre 1 y 65535, o con corchetes sin cerrar.
[[nodiscard]] std::optional<ParsedUrl> parse_url(std::string_view url);

/// true si el host es exactamente localhost, 127.0.0.1 o [::1].
[[nodiscard]] bool is_local_host(std::string_view host);

} // namespace chatbot

#endif // CHATBOT_URL_H
