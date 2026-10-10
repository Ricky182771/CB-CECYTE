#include "input_edit.h"

#include "chatbot/utf8.h"

#include <algorithm>
#include <cstddef>
#include <iterator>

namespace chatbot::cli {

namespace {

/// Del más largo al más corto: el primero que cabe.
constexpr std::string_view kPlaceholders[] = {
    "Escribe tu mensaje · Enter envía · \\ + Enter: nueva línea",
    "Escribe tu mensaje · \\ + Enter: nueva línea",
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

void insert_at_cursor(std::string& text, int& cursor, std::string_view piece) {
    const std::size_t at = clamped(text, cursor);
    text.insert(at, piece);
    cursor = static_cast<int>(at + piece.size());
}

int cursor_at_end(std::string_view text) { return static_cast<int>(text.size()); }

int input_height(std::string_view text, int terminal_rows) {
    const int lines = static_cast<int>(std::count(text.begin(), text.end(), '\n')) + 1;
    const int limit = std::max(1, std::min(kMaxInputLines, terminal_rows / 3));
    return std::min(lines, limit);
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
