// Enteros de los updates (telegram_integer): número de JSON o cadena de
// dígitos, como manda Telegram el draft_id de stopped_message_generation.

#include "integer.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace {

using chatbot::telegram::parse_decimal_int64;
using chatbot::telegram::telegram_integer;
using nlohmann::json;

constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
constexpr std::int64_t kMin = std::numeric_limits<std::int64_t>::min();

} // namespace

TEST_CASE("enteros: número de JSON", "[telegram][enteros]") {
    CHECK(telegram_integer(json(12345)) == std::optional<std::int64_t>{12345});
    CHECK(telegram_integer(json(-6000000042)) == std::optional<std::int64_t>{-6000000042});
    CHECK(telegram_integer(json(0)) == std::optional<std::int64_t>{0});
    CHECK(telegram_integer(json(kMax)) == std::optional<std::int64_t>{kMax});
    CHECK(telegram_integer(json(kMin)) == std::optional<std::int64_t>{kMin});
    // Un sin signo que no cabe en int64_t no se recorta.
    CHECK_FALSE(telegram_integer(json(static_cast<std::uint64_t>(kMax) + 1)).has_value());
    CHECK_FALSE(telegram_integer(json::parse("9223372036854775808")).has_value());
    CHECK_FALSE(telegram_integer(json(1.0)).has_value());
    CHECK_FALSE(telegram_integer(json::parse("1e3")).has_value());
}

TEST_CASE("enteros: cadena de dígitos", "[telegram][enteros]") {
    CHECK(telegram_integer(json("12345")) == std::optional<std::int64_t>{12345});
    CHECK(telegram_integer(json("-100777")) == std::optional<std::int64_t>{-100777});
    CHECK(telegram_integer(json("0")) == std::optional<std::int64_t>{0});
    CHECK(telegram_integer(json("9223372036854775807")) == std::optional<std::int64_t>{kMax});
    CHECK(telegram_integer(json("-9223372036854775808")) == std::optional<std::int64_t>{kMin});
    CHECK(parse_decimal_int64("449973635") == std::optional<std::int64_t>{449973635});
}

TEST_CASE("enteros: lo que no es un entero decimal se rechaza", "[telegram][enteros]") {
    for (const char* bad : {"", " ", " 1", "1 ", "1\n", "+1", "-", "--1", "1-", "1-2", "1.0",
                            "1e3", "0x10", "abc", "12a", "9223372036854775808",
                            "-9223372036854775809", "99999999999999999999", "١٢"}) {
        INFO('"' << bad << '"');
        CHECK_FALSE(parse_decimal_int64(bad).has_value());
        CHECK_FALSE(telegram_integer(json(bad)).has_value());
    }
    // Un cero en medio de la cadena: from_chars se detendría ahí.
    CHECK_FALSE(parse_decimal_int64(std::string_view{"12\0" "3", 4}).has_value());
}

TEST_CASE("enteros: null, booleano, objeto y arreglo no son enteros", "[telegram][enteros]") {
    CHECK_FALSE(telegram_integer(json(nullptr)).has_value());
    CHECK_FALSE(telegram_integer(json(true)).has_value());
    CHECK_FALSE(telegram_integer(json(false)).has_value());
    CHECK_FALSE(telegram_integer(json::object()).has_value());
    CHECK_FALSE(telegram_integer(json{{"id", 1}}).has_value());
    CHECK_FALSE(telegram_integer(json::array({1})).has_value());
}
