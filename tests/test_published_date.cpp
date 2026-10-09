#include "published_date.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

using chatbot::normalize_published_date;

TEST_CASE("fecha de publicación: ISO 8601", "[busqueda][fecha]") {
    CHECK(normalize_published_date("2026-10-04") == "2026-10-04");
    CHECK(normalize_published_date("2026-10-04T17:00:00Z") == "2026-10-04");
    CHECK(normalize_published_date("2026-10-04T23:30:00-06:00") == "2026-10-04");
    CHECK(normalize_published_date("2026-10-04 17:00:00") == "2026-10-04");
    CHECK(normalize_published_date("2024-02-29") == "2024-02-29");
}

TEST_CASE("fecha de publicación: RFC 1123", "[busqueda][fecha]") {
    // El ejemplo de la documentación de Tavily.
    CHECK(normalize_published_date("Tue, 11 Mar 2025 17:00:00 GMT") == "2025-03-11");
    CHECK(normalize_published_date("Sun, 04 Oct 2026 17:00:00 GMT") == "2026-10-04");
    CHECK(normalize_published_date("Sun, 4 Oct 2026 17:00:00 GMT") == "2026-10-04");
    CHECK(normalize_published_date("4 Oct 2026") == "2026-10-04");
    CHECK(normalize_published_date("Sun, 04 OCT 2026 17:00:00 GMT") == "2026-10-04");
    CHECK(normalize_published_date("Wed, 31 Dec 2025 23:59:59 +0000") == "2025-12-31");
}

TEST_CASE("fecha de publicación: vacía", "[busqueda][fecha]") {
    CHECK(normalize_published_date("").empty());
}

TEST_CASE("fecha de publicación: lo que no se reconoce queda tal cual", "[busqueda][fecha]") {
    for (const std::string date :
         {"hace 3 días", "ayer", "2026", "2026-10", "2026/10/04", "04/10/2026", "2026-10-04x",
          "2026-13-01", "2026-02-30", "2025-02-29", "2026-1-04", "Sun, 32 Oct 2026",
          "Sun, 04 Oct 26", "Sun, 04 Foo 2026", "Sunday, 04 Oct 2026", "04 Oct 2026x",
          " 2026-10-04", "\x01\xff"}) {
        INFO(date);
        CHECK(normalize_published_date(date) == date);
    }
}
