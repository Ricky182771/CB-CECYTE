#include "chatbot/tavily_search.h"
#include "chatbot/error.h"
#include "published_date.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace chatbot {
namespace {

using nlohmann::json;

/// Trunca el mensaje de error a un máximo de caracteres.
std::string truncate_message(std::string message, std::size_t max_length = 200) {
    if (message.size() <= max_length) {
        return message;
    }
    // Sin partir un carácter UTF-8: retrocede sobre los bytes de continuación.
    std::size_t end = max_length;
    while (end > 0 && (static_cast<unsigned char>(message[end]) & 0xC0) == 0x80) {
        --end;
    }
    message.resize(end);
    message += "…";
    return message;
}

/// Clasifica un error HTTP según el estado.
ErrorKind classify_http_error(int status) {
    if (status == 401) {
        return ErrorKind::Auth;
    }
    if (status == 400 || status == 422) {
        return ErrorKind::InvalidRequest;
    }
    if (status == 429 || status == 432 || status == 433) {
        return ErrorKind::RateLimited;
    }
    if (status >= 500 && status < 600) {
        return ErrorKind::Server;
    }
    return ErrorKind::BadResponse;
}

/// Extrae el mensaje de error del cuerpo JSON de Tavily.
/// Según la documentación oficial (https://docs.tavily.com/documentation/api-reference/endpoint/usage),
/// los errores siguen el formato: {"detail": {"error": "mensaje"}}.
std::string extract_error_message(const std::string& body, int status) {
    // Mensajes predeterminados específicos para Tavily tienen prioridad.
    if (status == 401) {
        return "La key de búsqueda no es válida";
    }
    if (status == 432 || status == 433) {
        return "Se agotaron las búsquedas del plan de Tavily";
    }

    // Intentar extraer del JSON: {"detail": {"error": "mensaje"}}.
    try {
        const json doc = json::parse(body);
        if (doc.contains("detail") && doc["detail"].is_object()) {
            const json& detail = doc["detail"];
            if (detail.contains("error") && detail["error"].is_string()) {
                return truncate_message(detail["error"].get<std::string>());
            }
        }
    } catch (...) {
        // Si no se puede parsear, usar mensaje genérico.
    }

    return "Error HTTP " + std::to_string(status);
}

/// Valida que una URL sea http:// o https://.
bool is_valid_url(std::string_view url) {
    return url.size() >= 8 &&
           (url.substr(0, 8) == "https://" || url.substr(0, 7) == "http://");
}

}  // namespace

class TavilySearch::Impl {
public:
    Impl(std::string api_key, std::unique_ptr<Transport> transport)
        : api_key_{std::move(api_key)}, transport_{std::move(transport)} {}

    Result<SearchResponse> search(std::string_view query, const CancelToken* cancel) {
        // Armar el cuerpo de la petición.
        json request_body = {
            {"query", query},
            {"search_depth", "basic"},
            {"max_results", 5},
            {"topic", "general"},
            {"safe_search", true},
            {"country", "mexico"},
            {"include_published_date", true}
        };

        HttpRequest request;
        request.url = "https://api.tavily.com/search";
        request.method = HttpMethod::Post;
        request.api_key = api_key_;
        request.body = request_body.dump();
        request.cancel = cancel;
        request.timeout = std::chrono::seconds{30};

        // Enviar la petición.
        const HttpResponse response = transport_->send(request);

        // Cancelación: la del token gana aunque la petición haya terminado.
        if (response.cancelled || (cancel != nullptr && cancel->is_cancelled())) {
            return ChatError{ErrorKind::Cancelled, 0, "Búsqueda cancelada.", std::nullopt};
        }

        // Sin respuesta HTTP (status 0): fallo de red o tiempo agotado, nunca
        // "Error HTTP 0", aunque el transporte no haya dado una descripción.
        if (response.status == 0) {
            const ErrorKind kind = response.timed_out ? ErrorKind::Timeout : ErrorKind::Network;
            std::string message = response.error;
            if (message.empty()) {
                message = response.timed_out
                              ? "Se agotó el tiempo de espera del servicio de búsqueda."
                              : "No se pudo conectar con el servicio de búsqueda.";
            }
            return ChatError{kind, 0, std::move(message), std::nullopt};
        }

        // Manejar errores HTTP.
        if (response.status < 200 || response.status >= 300) {
            const ErrorKind kind = classify_http_error(response.status);
            const std::string message = extract_error_message(response.body, response.status);
            return ChatError{kind, response.status, message, std::nullopt};
        }

        // Parsear la respuesta.
        json doc;
        try {
            doc = json::parse(response.body);
        } catch (const json::exception&) {
            return ChatError{ErrorKind::BadResponse, response.status,
                           "La respuesta no es JSON válido", std::nullopt};
        }

        if (!doc.contains("results") || !doc["results"].is_array()) {
            return ChatError{ErrorKind::BadResponse, response.status,
                           "La respuesta no contiene el campo \"results\"", std::nullopt};
        }

        // Extraer los resultados.
        SearchResponse search_response;
        search_response.query = std::string{query};

        for (const auto& item : doc["results"]) {
            if (!item.is_object()) {
                continue;  // Ignorar resultados mal formados.
            }

            SearchResult result;

            // URL es obligatoria y debe ser válida.
            if (!item.contains("url") || !item["url"].is_string()) {
                continue;
            }
            result.url = item["url"].get<std::string>();
            if (!is_valid_url(result.url)) {
                continue;  // Descartar URLs no HTTP/HTTPS.
            }

            // content es obligatorio.
            if (!item.contains("content") || !item["content"].is_string()) {
                continue;
            }
            result.content = item["content"].get<std::string>();

            // title y published_date son opcionales.
            if (item.contains("title") && item["title"].is_string()) {
                result.title = item["title"].get<std::string>();
            }
            // Llega como RFC 1123 ("Tue, 11 Mar 2025 17:00:00 GMT") o null.
            if (item.contains("published_date") && item["published_date"].is_string()) {
                result.published_date =
                    normalize_published_date(item["published_date"].get<std::string>());
            }

            search_response.results.push_back(std::move(result));
        }

        return search_response;
    }

private:
    std::string api_key_;
    std::unique_ptr<Transport> transport_;
};

TavilySearch::TavilySearch(std::string api_key, std::unique_ptr<Transport> transport)
    : impl_{std::make_unique<Impl>(std::move(api_key), std::move(transport))} {}

TavilySearch::~TavilySearch() = default;

Result<SearchResponse> TavilySearch::search(std::string_view query, const CancelToken* cancel) {
    return impl_->search(query, cancel);
}

}  // namespace chatbot
