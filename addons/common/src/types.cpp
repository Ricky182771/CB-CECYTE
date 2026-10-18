#include "chatbot/bot/types.h"

#include "chatbot/utf8.h"

#include <cstdint>
#include <optional>

namespace chatbot::bot {

std::size_t utf16_length(std::string_view utf8) {
    std::size_t units = 0;
    std::size_t i = 0;
    while (i < utf8.size()) {
        const std::optional<std::uint32_t> code = utf8::next_code_point(utf8, i);
        if (!code.has_value()) {
            ++i; // Byte inválido: U+FFFD, una unidad.
            ++units;
            continue;
        }
        units += *code > 0xFFFF ? 2 : 1;
    }
    return units;
}

} // namespace chatbot::bot
