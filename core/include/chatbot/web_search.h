#ifndef CHATBOT_WEB_SEARCH_H
#define CHATBOT_WEB_SEARCH_H

#include "chatbot/result.h"
#include "chatbot/cancel_token.h"

#include <string>
#include <string_view>
#include <vector>

namespace chatbot {

/// Un resultado de búsqueda web.
struct SearchResult {
    std::string title;
    std::string url;
    std::string content;
    std::string published_date;  // Vacío si no está disponible.
};

/// Respuesta de una búsqueda web.
struct SearchResponse {
    std::string query;
    std::vector<SearchResult> results;
};

/// Proveedor de búsqueda web (interfaz).
class SearchProvider {
public:
    virtual ~SearchProvider() = default;

    /// Busca en la web. Devuelve los resultados o un error.
    [[nodiscard]] virtual Result<SearchResponse> search(
        std::string_view query,
        const CancelToken* cancel = nullptr) = 0;
};

/// Formatea los resultados de búsqueda como contexto para el modelo.
///
/// @param response Los resultados de la búsqueda.
/// @param today La fecha de hoy en formato legible (por ejemplo, "6 de octubre de 2026").
/// @param nonce Cadena aleatoria para evitar inyección de cierre de etiqueta.
/// @return El bloque de contexto formateado.
std::string format_search_context(const SearchResponse& response,
                                  std::string_view today,
                                  std::string_view nonce);

}  // namespace chatbot

#endif  // CHATBOT_WEB_SEARCH_H
