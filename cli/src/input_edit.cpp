#include "input_edit.h"

#include "chatbot/utf8.h"

#include <algorithm>
#include <cstddef>
#include <iterator>

namespace chatbot::cli {

namespace {

/// Del más largo al más corto: el primero que cabe.
constexpr std::string_view kPlaceholders[] = {
    "Escribe tu mensaje · Enter envía · \\ + Enter: nueva línea · / comandos",
    "Escribe tu mensaje · \\ + Enter: nueva línea · / comandos",
    "Mensaje · \\ + Enter: nueva línea · / comandos",
    "Mensaje · \\ + Enter: nueva línea",
    "Escribe tu mensaje",
};

/// Columnas de un texto de caracteres angostos (los placeholders lo son).
int columns_of(std::string_view text) {
    int count = 0;
    std::size_t i = 0;
    while (i < text.size()) {
        if (!utf8::next_code_point(text, i).has_value()) {
            ++i;
        }
        ++count;
    }
    return count;
}

/// El cursor acotado a [0, text.size()].
std::size_t clamped(std::string_view text, int cursor) {
    return static_cast<std::size_t>(std::clamp(cursor, 0, static_cast<int>(text.size())));
}

} // namespace

bool backslash_newline(std::string& text, int& cursor) {
    const std::size_t at = clamped(text, cursor);
    if (at == 0 || text[at - 1] != '\\') {
        return false;
    }
    text[at - 1] = '\n';
    cursor = static_cast<int>(at);
    return true;
}

int input_max_rows(int terminal_rows) {
    return std::max(1, std::min(kMaxInputLines, terminal_rows / 3));
}

std::string_view input_placeholder(int columns) {
    for (const std::string_view candidate : kPlaceholders) {
        if (columns_of(candidate) <= columns) {
            return candidate;
        }
    }
    return kPlaceholders[std::size(kPlaceholders) - 1];
}

} // namespace chatbot::cli
