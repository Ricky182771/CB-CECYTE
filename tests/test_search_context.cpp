#include "search_context.h"

#include <catch2/catch_test_macros.hpp>

#include <ctime>
#include <set>
#include <string>

using chatbot::cli::generate_search_nonce;
using chatbot::cli::local_iso_date;
using chatbot::cli::spanish_date;

TEST_CASE("spanish_date: AAAA-MM-DD en español", "[busqueda][fecha]") {
    CHECK(spanish_date("2026-10-08") == "8 de octubre de 2026");
    CHECK(spanish_date("2026-01-31") == "31 de enero de 2026");
    CHECK(spanish_date("2025-12-01") == "1 de diciembre de 2025");
    // Una fecha ISO 8601 completa (created_at) también sirve.
    CHECK(spanish_date("2026-10-02T23:58:00-06:00") == "2 de octubre de 2026");
}

TEST_CASE("spanish_date: lo que no es una fecha se devuelve tal cual", "[busqueda][fecha]") {
    CHECK(spanish_date("") == "fecha desconocida");
    CHECK(spanish_date("ayer") == "ayer");
    CHECK(spanish_date("2026-13-01") == "2026-13-01");
    CHECK(spanish_date("2026-10-0x") == "2026-10-0x");
    CHECK(spanish_date("2026-10-08x") == "2026-10-08x");
}

TEST_CASE("local_iso_date: forma AAAA-MM-DD y vuelve a spanish_date", "[busqueda][fecha]") {
    const std::string today = local_iso_date(std::time(nullptr));
    REQUIRE(today.size() == 10);
    CHECK(today[4] == '-');
    CHECK(today[7] == '-');
    CHECK(spanish_date(today).find(" de ") != std::string::npos);
}

TEST_CASE("generate_search_nonce: 16 hexadecimales y distinto cada vez", "[busqueda]") {
    std::set<std::string> seen;
    for (int i = 0; i < 100; ++i) {
        const std::string nonce = generate_search_nonce();
        REQUIRE(nonce.size() == 16);
        CHECK(nonce.find_first_not_of("0123456789abcdef") == std::string::npos);
        seen.insert(nonce);
    }
    CHECK(seen.size() == 100);
}
