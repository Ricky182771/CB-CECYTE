#include "search_context.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <random>

namespace chatbot::cli {

std::string generate_search_nonce() {
    std::random_device device;
    const std::uint64_t value =
        (static_cast<std::uint64_t>(device()) << 32) | static_cast<std::uint64_t>(device());
    std::array<char, 17> buffer{};
    std::snprintf(buffer.data(), buffer.size(), "%016llx",
                  static_cast<unsigned long long>(value));
    return std::string{buffer.data()};
}

std::string local_iso_date(std::time_t time) {
    std::tm local{};
    if (localtime_r(&time, &local) == nullptr) {
        return {};
    }
    std::array<char, 16> buffer{};
    if (std::strftime(buffer.data(), buffer.size(), "%Y-%m-%d", &local) == 0) {
        return {};
    }
    return std::string{buffer.data()};
}

std::string spanish_date(std::string_view iso_date) {
    static constexpr std::array<const char*, 12> kMonths = {
        "enero", "febrero", "marzo",      "abril",   "mayo",      "junio",
        "julio", "agosto",  "septiembre", "octubre", "noviembre", "diciembre"};
    if (iso_date.empty()) {
        return "fecha desconocida";
    }
    const auto number = [iso_date](std::size_t at, std::size_t length) {
        int value = 0;
        for (std::size_t i = at; i < at + length; ++i) {
            if (iso_date[i] < '0' || iso_date[i] > '9') {
                return -1;
            }
            value = value * 10 + (iso_date[i] - '0');
        }
        return value;
    };
    if (iso_date.size() < 10 || iso_date[4] != '-' || iso_date[7] != '-' ||
        (iso_date.size() > 10 && iso_date[10] != 'T')) {
        return std::string{iso_date};
    }
    const int year = number(0, 4);
    const int month = number(5, 2);
    const int day = number(8, 2);
    if (year < 0 || month < 1 || month > 12 || day < 1 || day > 31) {
        return std::string{iso_date};
    }
    return std::to_string(day) + " de " + kMonths[static_cast<std::size_t>(month - 1)] + " de " +
           std::to_string(year);
}

} // namespace chatbot::cli
