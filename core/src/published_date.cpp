#include "published_date.h"

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace chatbot {
namespace {

bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

/// Lee exactamente `count` dígitos desde `pos`. nullopt si alguno no lo es.
std::optional<int> read_digits(std::string_view text, std::size_t pos, std::size_t count) {
    if (pos + count > text.size()) {
        return std::nullopt;
    }
    int value = 0;
    for (std::size_t i = pos; i < pos + count; ++i) {
        if (!is_digit(text[i])) {
            return std::nullopt;
        }
        value = value * 10 + (text[i] - '0');
    }
    return value;
}

bool is_leap(int year) {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

bool is_valid_date(int year, int month, int day) {
    static constexpr std::array<int, 12> kDays{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12 || day < 1) {
        return false;
    }
    const int days = month == 2 && is_leap(year) ? 29 : kDays[static_cast<std::size_t>(month - 1)];
    return day <= days;
}

std::string format_date(int year, int month, int day) {
    std::string out(10, '0');
    out[0] = static_cast<char>('0' + year / 1000);
    out[1] = static_cast<char>('0' + year / 100 % 10);
    out[2] = static_cast<char>('0' + year / 10 % 10);
    out[3] = static_cast<char>('0' + year % 10);
    out[4] = '-';
    out[5] = static_cast<char>('0' + month / 10);
    out[6] = static_cast<char>('0' + month % 10);
    out[7] = '-';
    out[8] = static_cast<char>('0' + day / 10);
    out[9] = static_cast<char>('0' + day % 10);
    return out;
}

/// "AAAA-MM-DD", sola o seguida de 'T' o de un espacio.
std::optional<std::string> parse_iso(std::string_view text) {
    const std::optional<int> year = read_digits(text, 0, 4);
    const std::optional<int> month = read_digits(text, 5, 2);
    const std::optional<int> day = read_digits(text, 8, 2);
    if (!year || !month || !day || text[4] != '-' || text[7] != '-') {
        return std::nullopt;
    }
    if (text.size() > 10 && text[10] != 'T' && text[10] != ' ') {
        return std::nullopt;
    }
    if (!is_valid_date(*year, *month, *day)) {
        return std::nullopt;
    }
    return format_date(*year, *month, *day);
}

char lower(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

/// Número de mes (1–12) de su abreviatura en inglés, o 0.
int month_number(std::string_view name) {
    static constexpr std::array<std::string_view, 12> kMonths{
        "jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec"};
    if (name.size() != 3) {
        return 0;
    }
    for (std::size_t i = 0; i < kMonths.size(); ++i) {
        if (lower(name[0]) == kMonths[i][0] && lower(name[1]) == kMonths[i][1] &&
            lower(name[2]) == kMonths[i][2]) {
            return static_cast<int>(i) + 1;
        }
    }
    return 0;
}

/// "[Ddd, ]D[D] Mmm AAAA[ hora...]".
std::optional<std::string> parse_rfc1123(std::string_view text) {
    std::size_t pos = 0;
    const std::size_t comma = text.find(',');
    if (comma != std::string_view::npos) {
        if (comma != 3 || comma + 1 >= text.size() || text[comma + 1] != ' ') {
            return std::nullopt;
        }
        pos = comma + 2;
    }
    std::size_t day_digits = 0;
    while (pos + day_digits < text.size() && is_digit(text[pos + day_digits])) {
        ++day_digits;
    }
    if (day_digits != 1 && day_digits != 2) {
        return std::nullopt;
    }
    const std::optional<int> day = read_digits(text, pos, day_digits);
    pos += day_digits;
    if (pos >= text.size() || text[pos] != ' ') {
        return std::nullopt;
    }
    ++pos;
    const int month = month_number(text.substr(pos, 3));
    pos += 3;
    if (month == 0 || pos >= text.size() || text[pos] != ' ') {
        return std::nullopt;
    }
    ++pos;
    const std::optional<int> year = read_digits(text, pos, 4);
    pos += 4;
    if (!year || (pos < text.size() && text[pos] != ' ')) {
        return std::nullopt;
    }
    if (!is_valid_date(*year, month, *day)) {
        return std::nullopt;
    }
    return format_date(*year, month, *day);
}

} // namespace

std::string normalize_published_date(std::string_view date) {
    if (std::optional<std::string> iso = parse_iso(date)) {
        return std::move(*iso);
    }
    if (std::optional<std::string> rfc = parse_rfc1123(date)) {
        return std::move(*rfc);
    }
    return std::string{date};
}

} // namespace chatbot
