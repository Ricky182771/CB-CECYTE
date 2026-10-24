#include "chatbot/bot/types.h"

#include "chatbot/utf8.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <random>

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

ReplyId to_reply_range(ReplyId value) {
    if (value >= 1 && value <= kMaxReplyId) {
        return value;
    }
    const auto range = static_cast<std::uint64_t>(kMaxReplyId);
    return static_cast<ReplyId>(static_cast<std::uint64_t>(value) % range + 1);
}

ReplyId random_reply_id() {
    try {
        std::random_device device;
        std::uniform_int_distribution<ReplyId> distribution(1, kMaxReplyId);
        return distribution(device);
    } catch (...) {
        // random_device puede lanzar si no hay fuente de entropía: el reloj
        // en nanosegundos, revuelto (los bits de abajo cambian más rápido).
        auto ticks = static_cast<std::uint64_t>(
            std::chrono::system_clock::now().time_since_epoch().count());
        ticks ^= ticks >> 31;
        ticks *= 0x9E3779B97F4A7C15ULL;
        ticks ^= ticks >> 29;
        return to_reply_range(static_cast<ReplyId>(ticks & 0x7FFFFFFFFFFFFFFFULL));
    }
}

} // namespace chatbot::bot
