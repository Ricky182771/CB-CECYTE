#ifndef CHATBOT_WEB_SEARCH_H
#define CHATBOT_WEB_SEARCH_H

#include "chatbot/result.h"
#include "chatbot/cancel_token.h"

#include <cstddef>
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

/// Máximo de bytes de content por resultado en el bloque de contexto.
inline constexpr std::size_t kSearchContentMaxBytes = 1200;
/// Máximo de bytes de content, sumando todos los resultados.
inline constexpr std::size_t kSearchTotalMaxBytes = 6000;

/// Deja los resultados como entran al bloque de contexto: content sin
/// controles C0 (salvo \n; \t se vuelve espacio), DEL ni C1, con cada byte
/// inválido como U+FFFD, y recortado a kSearchContentMaxBytes por resultado y
/// kSearchTotalMaxBytes en total, sin partir caracteres UTF-8; los resultados
/// que ya no caben se quitan. query, title, url y published_date no cambian. Aplicarla otra vez
/// al resultado no cambia nada: es lo que se guarda con la conversación.
[[nodiscard]] SearchResponse trim_search_response(const SearchResponse& response);

/// Formatea los resultados de búsqueda como contexto para el modelo. Recorta
/// con trim_search_response; en title, url, published_date, search_date y la
/// consulta, además, \n se vuelve espacio. La cadena de cierre del bloque se
/// neutraliza en todos los campos.
///
/// @param response Los resultados de la búsqueda.
/// @param search_date La fecha en que se hizo la búsqueda, en formato legible
///                    (por ejemplo, "6 de octubre de 2026"); va como "Fecha de la
///                    búsqueda". Al reabrir una conversación es la guardada.
/// @param nonce Cadena aleatoria para evitar inyección de cierre de etiqueta.
/// @return El bloque de contexto formateado.
[[nodiscard]] std::string format_search_context(const SearchResponse& response,
                                                std::string_view search_date,
                                                std::string_view nonce);

}  // namespace chatbot

#endif  // CHATBOT_WEB_SEARCH_H
