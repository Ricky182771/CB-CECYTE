#ifndef CHATBOT_CLI_SEARCH_CONTEXT_H
#define CHATBOT_CLI_SEARCH_CONTEXT_H

#include <ctime>
#include <string>
#include <string_view>

namespace chatbot::cli {

/// Mensaje cuando la búsqueda no trae resultados: no se llama al modelo.
inline constexpr std::string_view kNoSearchResults = "La búsqueda no encontró resultados.";

/// Aviso cuando no hay key de búsqueda: /buscar no se envía.
inline constexpr std::string_view kMissingSearchKey =
    "Configura la key de búsqueda en Configuración (F2) → Búsqueda web";

/// Nonce del bloque de resultados: 16 caracteres hexadecimales aleatorios,
/// uno por bloque. Seguro entre hilos.
[[nodiscard]] std::string generate_search_nonce();

/// Fecha local "AAAA-MM-DD": la que se guarda con la búsqueda. Vacía si no
/// se puede convertir.
[[nodiscard]] std::string local_iso_date(std::time_t time);

/// "AAAA-MM-DD" (también el inicio de una fecha ISO 8601 completa) en
/// español para "Fecha de la búsqueda": "8 de octubre de 2026". Si no tiene esa
/// forma, la devuelve tal cual; vacía, "fecha desconocida".
[[nodiscard]] std::string spanish_date(std::string_view iso_date);

} // namespace chatbot::cli

#endif // CHATBOT_CLI_SEARCH_CONTEXT_H
