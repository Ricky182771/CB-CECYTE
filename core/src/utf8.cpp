#include "chatbot/utf8.h"

namespace chatbot::utf8 {

std::optional<std::uint32_t> next_code_point(std::string_view text, std::size_t& i) {
    const auto byte = [&text](std::size_t k) { return static_cast<unsigned char>(text[k]); };
    const unsigned char lead = byte(i);
    std::size_t length = 0;
    std::uint32_t code = 0;
    std::uint32_t minimum = 0;
    if (lead < 0x80) {
        ++i;
        return lead;
    }
    if ((lead & 0xE0) == 0xC0) {
        length = 2;
        code = lead & 0x1Fu;
        minimum = 0x80;
    } else if ((lead & 0xF0) == 0xE0) {
        length = 3;
        code = lead & 0x0Fu;
        minimum = 0x800;
    } else if ((lead & 0xF8) == 0xF0) {
        length = 4;
        code = lead & 0x07u;
        minimum = 0x10000;
    } else {
        return std::nullopt;
    }
    if (text.size() - i < length) {
        return std::nullopt;
    }
    for (std::size_t k = 1; k < length; ++k) {
        const unsigned char continuation = byte(i + k);
        if ((continuation & 0xC0) != 0x80) {
            return std::nullopt;
        }
        code = (code << 6) | (continuation & 0x3Fu);
    }
    if (code < minimum || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) {
        return std::nullopt;
    }
    i += length;
    return code;
}

bool is_valid(std::string_view text) {
    std::size_t i = 0;
    while (i < text.size()) {
        if (!next_code_point(text, i).has_value()) {
            return false;
        }
    }
    return true;
}

} // namespace chatbot::utf8
