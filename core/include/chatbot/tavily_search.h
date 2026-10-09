#ifndef CHATBOT_TAVILY_SEARCH_H
#define CHATBOT_TAVILY_SEARCH_H

#include "chatbot/web_search.h"
#include "chatbot/transport.h"

#include <memory>
#include <string>

namespace chatbot {

/// Implementación de SearchProvider usando la API de Tavily.
class TavilySearch final : public SearchProvider {
public:
    /// Construye un cliente de Tavily.
    ///
    /// @param api_key La API key de Tavily.
    /// @param transport El transporte HTTP (CurlTransport en producción,
    ///                  FakeTransport en pruebas).
    TavilySearch(std::string api_key, std::unique_ptr<Transport> transport);

    ~TavilySearch() override;

    TavilySearch(const TavilySearch&) = delete;
    TavilySearch& operator=(const TavilySearch&) = delete;

    [[nodiscard]] Result<SearchResponse> search(
        std::string_view query,
        const CancelToken* cancel = nullptr) override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace chatbot

#endif  // CHATBOT_TAVILY_SEARCH_H
