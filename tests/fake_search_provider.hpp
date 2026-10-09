#ifndef CHATBOT_TEST_FAKE_SEARCH_PROVIDER_HPP
#define CHATBOT_TEST_FAKE_SEARCH_PROVIDER_HPP

#include "chatbot/transport.h"

#include <nlohmann/json.hpp>

namespace chatbot_test {

/// Respuesta 200 de Tavily con los resultados dados (título, url, contenido).
/// Con un FakeTransport, hace de proveedor de búsqueda falso para
/// TavilySearch, sin red.
inline chatbot::HttpResponse tavily_response(const nlohmann::json& results) {
    chatbot::HttpResponse response;
    response.status = 200;
    response.body = nlohmann::json{{"query", "q"}, {"results", results}}.dump();
    return response;
}

} // namespace chatbot_test

#endif // CHATBOT_TEST_FAKE_SEARCH_PROVIDER_HPP
