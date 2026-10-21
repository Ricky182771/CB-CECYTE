#include "integer.h"

#include <charconv>
#include <limits>
#include <string>
#include <system_error>

namespace chatbot::telegram {

std::optional<std::int64_t> parse_decimal_int64(std::string_view text) {
    std::int64_t value = 0;
    const char* const end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(text.data(), end, value, 10);
    if (ec != std::errc{} || ptr != end) {
        return std::nullopt;
    }
    return value;
}

std::optional<std::int64_t> telegram_integer(const nlohmann::json& value) {
    if (value.is_number_unsigned()) {
        const auto number = value.get<std::uint64_t>();
        if (number > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            return std::nullopt;
        }
        return static_cast<std::int64_t>(number);
    }
    if (value.is_number_integer()) {
        return value.get<std::int64_t>();
    }
    if (value.is_string()) {
        return parse_decimal_int64(value.get_ref<const std::string&>());
    }
    return std::nullopt;
}

} // namespace chatbot::telegram
