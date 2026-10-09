#include <catch2/catch_test_macros.hpp>

#include "chatbot/tavily_search.h"
#include "chatbot/error.h"
#include "fake_transport.hpp"

#include <nlohmann/json.hpp>

#include <memory>
#include <string>

using namespace chatbot;
using chatbot_test::FakeTransport;
using nlohmann::json;

TEST_CASE("TavilySearch envía el cuerpo correcto", "[tavily_search]") {
    auto transport = std::make_unique<FakeTransport>();
    FakeTransport* raw = transport.get();

    TavilySearch search{"test-key", std::move(transport)};

    json response_body = {
        {"results", json::array({
            {{"title", "Resultado 1"},
             {"url", "https://ejemplo.com/1"},
             {"content", "Contenido del resultado 1"}}
        })}
    };
    HttpResponse resp;
    resp.status = 200;
    resp.body = response_body.dump();
    raw->responses.push_back(resp);

    const auto result = search.search("prueba de búsqueda", nullptr);
    REQUIRE(result.is_ok());

    // Verificar el cuerpo de la petición.
    REQUIRE(raw->requests.size() == 1);
    const HttpRequest& req = raw->requests[0];

    REQUIRE(req.url == "https://api.tavily.com/search");
    REQUIRE(req.method == HttpMethod::Post);
    REQUIRE(req.api_key == "test-key");

    const json request_body = json::parse(req.body);
    REQUIRE(request_body["query"] == "prueba de búsqueda");
    REQUIRE(request_body["search_depth"] == "basic");
    REQUIRE(request_body["max_results"] == 5);
    REQUIRE(request_body["topic"] == "general");
    REQUIRE(request_body["safe_search"] == true);
    REQUIRE(request_body["country"] == "mexico");

    // La key va solo en api_key, no en el cuerpo.
    REQUIRE_FALSE(request_body.contains("api_key"));
}

TEST_CASE("TavilySearch parsea una respuesta válida", "[tavily_search]") {
    auto transport = std::make_unique<FakeTransport>();
    FakeTransport* raw = transport.get();

    TavilySearch search{"test-key", std::move(transport)};

    json response_body = {
        {"results", json::array({
            {{"title", "Título 1"},
             {"url", "https://ejemplo.com/1"},
             {"content", "Contenido 1"},
             {"published_date", "2026-10-01"}},
            {{"title", "Título 2"},
             {"url", "http://localhost/2"},
             {"content", "Contenido 2"}}
        })}
    };
    HttpResponse resp;
    resp.status = 200;
    resp.body = response_body.dump();
    raw->responses.push_back(resp);

    const auto result = search.search("consulta", nullptr);
    REQUIRE(result.is_ok());

    const SearchResponse& response = result.value();
    REQUIRE(response.query == "consulta");
    REQUIRE(response.results.size() == 2);

    REQUIRE(response.results[0].title == "Título 1");
    REQUIRE(response.results[0].url == "https://ejemplo.com/1");
    REQUIRE(response.results[0].content == "Contenido 1");
    REQUIRE(response.results[0].published_date == "2026-10-01");

    REQUIRE(response.results[1].title == "Título 2");
    REQUIRE(response.results[1].url == "http://localhost/2");
    REQUIRE(response.results[1].content == "Contenido 2");
    REQUIRE(response.results[1].published_date.empty());
}

TEST_CASE("TavilySearch sin results da BadResponse", "[tavily_search]") {
    auto transport = std::make_unique<FakeTransport>();
    FakeTransport* raw = transport.get();

    TavilySearch search{"test-key", std::move(transport)};

    HttpResponse resp;
    resp.status = 200;
    resp.body = R"({"query": "test"})";
    raw->responses.push_back(resp);

    const auto result = search.search("test", nullptr);
    REQUIRE(result.is_error());
    REQUIRE(result.error().kind == ErrorKind::BadResponse);
}

TEST_CASE("TavilySearch con results vacío devuelve array vacío", "[tavily_search]") {
    auto transport = std::make_unique<FakeTransport>();
    FakeTransport* raw = transport.get();

    TavilySearch search{"test-key", std::move(transport)};

    HttpResponse resp;
    resp.status = 200;
    resp.body = R"({"results": []})";
    raw->responses.push_back(resp);

    const auto result = search.search("test", nullptr);
    REQUIRE(result.is_ok());
    REQUIRE(result.value().results.empty());
}

TEST_CASE("TavilySearch descarta resultados inválidos", "[tavily_search]") {
    auto transport = std::make_unique<FakeTransport>();
    FakeTransport* raw = transport.get();

    TavilySearch search{"test-key", std::move(transport)};

    json response_body = {
        {"results", json::array({
            // Sin url → descartado.
            {{"title", "Sin URL"}, {"content", "Contenido"}},
            // Sin content → descartado.
            {{"title", "Sin content"}, {"url", "https://ejemplo.com"}},
            // URL javascript: → descartado.
            {{"title", "JavaScript"}, {"url", "javascript:alert(1)"}, {"content", "X"}},
            // Válido.
            {{"title", "Válido"}, {"url", "https://ejemplo.com/ok"}, {"content", "OK"}}
        })}
    };
    HttpResponse resp;
    resp.status = 200;
    resp.body = response_body.dump();
    raw->responses.push_back(resp);

    const auto result = search.search("test", nullptr);
    REQUIRE(result.is_ok());
    REQUIRE(result.value().results.size() == 1);
    REQUIRE(result.value().results[0].title == "Válido");
}

TEST_CASE("TavilySearch maneja errores HTTP", "[tavily_search]") {
    SECTION("401 → Auth") {
        auto transport = std::make_unique<FakeTransport>();
        FakeTransport* raw = transport.get();
        TavilySearch search{"test-key", std::move(transport)};

        HttpResponse resp;
        resp.status = 401;
        resp.body = R"({"detail": {"error": "Unauthorized: missing or invalid API key."}})";
        raw->responses.push_back(resp);

        const auto result = search.search("test", nullptr);
        REQUIRE(result.is_error());
        REQUIRE(result.error().kind == ErrorKind::Auth);
        REQUIRE(result.error().message == "La key de búsqueda no es válida");
    }

    SECTION("400 → InvalidRequest") {
        auto transport = std::make_unique<FakeTransport>();
        FakeTransport* raw = transport.get();
        TavilySearch search{"test-key", std::move(transport)};

        HttpResponse resp;
        resp.status = 400;
        resp.body = R"({"detail": {"error": "Bad request"}})";
        raw->responses.push_back(resp);

        const auto result = search.search("test", nullptr);
        REQUIRE(result.is_error());
        REQUIRE(result.error().kind == ErrorKind::InvalidRequest);
        REQUIRE(result.error().message == "Bad request");
    }

    SECTION("422 → InvalidRequest") {
        auto transport = std::make_unique<FakeTransport>();
        FakeTransport* raw = transport.get();
        TavilySearch search{"test-key", std::move(transport)};

        HttpResponse resp;
        resp.status = 422;
        resp.body = "{}";
        raw->responses.push_back(resp);

        const auto result = search.search("test", nullptr);
        REQUIRE(result.is_error());
        REQUIRE(result.error().kind == ErrorKind::InvalidRequest);
    }

    SECTION("429 → RateLimited") {
        auto transport = std::make_unique<FakeTransport>();
        FakeTransport* raw = transport.get();
        TavilySearch search{"test-key", std::move(transport)};

        HttpResponse resp;
        resp.status = 429;
        resp.body = "{}";
        raw->responses.push_back(resp);

        const auto result = search.search("test", nullptr);
        REQUIRE(result.is_error());
        REQUIRE(result.error().kind == ErrorKind::RateLimited);
    }

    SECTION("432 → RateLimited con mensaje específico") {
        auto transport = std::make_unique<FakeTransport>();
        FakeTransport* raw = transport.get();
        TavilySearch search{"test-key", std::move(transport)};

        HttpResponse resp;
        resp.status = 432;
        resp.body = "{}";
        raw->responses.push_back(resp);

        const auto result = search.search("test", nullptr);
        REQUIRE(result.is_error());
        REQUIRE(result.error().kind == ErrorKind::RateLimited);
        REQUIRE(result.error().message == "Se agotaron las búsquedas del plan de Tavily");
    }

    SECTION("433 → RateLimited con mensaje específico") {
        auto transport = std::make_unique<FakeTransport>();
        FakeTransport* raw = transport.get();
        TavilySearch search{"test-key", std::move(transport)};

        HttpResponse resp;
        resp.status = 433;
        resp.body = "{}";
        raw->responses.push_back(resp);

        const auto result = search.search("test", nullptr);
        REQUIRE(result.is_error());
        REQUIRE(result.error().kind == ErrorKind::RateLimited);
        REQUIRE(result.error().message == "Se agotaron las búsquedas del plan de Tavily");
    }

    SECTION("500 → Server") {
        auto transport = std::make_unique<FakeTransport>();
        FakeTransport* raw = transport.get();
        TavilySearch search{"test-key", std::move(transport)};

        HttpResponse resp;
        resp.status = 500;
        resp.body = "{}";
        raw->responses.push_back(resp);

        const auto result = search.search("test", nullptr);
        REQUIRE(result.is_error());
        REQUIRE(result.error().kind == ErrorKind::Server);
    }
}

TEST_CASE("TavilySearch maneja JSON malformado", "[tavily_search]") {
    auto transport = std::make_unique<FakeTransport>();
    FakeTransport* raw = transport.get();

    TavilySearch search{"test-key", std::move(transport)};

    HttpResponse resp;
    resp.status = 200;
    resp.body = "esto no es JSON";
    raw->responses.push_back(resp);

    const auto result = search.search("test", nullptr);
    REQUIRE(result.is_error());
    REQUIRE(result.error().kind == ErrorKind::BadResponse);
}

TEST_CASE("TavilySearch maneja cancelación", "[tavily_search]") {
    auto transport = std::make_unique<FakeTransport>();
    FakeTransport* raw = transport.get();

    TavilySearch search{"test-key", std::move(transport)};

    CancelToken cancel;
    cancel.cancel();

    HttpResponse resp;
    resp.cancelled = true;
    raw->responses.push_back(resp);

    const auto result = search.search("test", &cancel);
    REQUIRE(result.is_error());
    REQUIRE(result.error().kind == ErrorKind::Cancelled);
}

TEST_CASE("TavilySearch maneja errores de red", "[tavily_search]") {
    SECTION("Error de red → Network") {
        auto transport = std::make_unique<FakeTransport>();
        FakeTransport* raw = transport.get();
        TavilySearch search{"test-key", std::move(transport)};

        HttpResponse resp;
        resp.status = 0;
        resp.body = "";
        resp.error = "Connection failed";
        resp.retryable = true;
        raw->responses.push_back(resp);

        const auto result = search.search("test", nullptr);
        REQUIRE(result.is_error());
        REQUIRE(result.error().kind == ErrorKind::Network);
    }

    SECTION("Timeout → Timeout") {
        auto transport = std::make_unique<FakeTransport>();
        FakeTransport* raw = transport.get();
        TavilySearch search{"test-key", std::move(transport)};

        HttpResponse resp;
        resp.status = 0;
        resp.body = "";
        resp.timed_out = true;
        resp.error = "Operation timed out";
        raw->responses.push_back(resp);

        const auto result = search.search("test", nullptr);
        REQUIRE(result.is_error());
        REQUIRE(result.error().kind == ErrorKind::Timeout);
    }
}
