#include "paste.h"

#include "chatbot/utf8.h"

#include <cstdint>

namespace chatbot::cli {

namespace {

/// Espacios que reemplazan a un tabulador (igual que md::sanitize).
constexpr std::string_view kTabSpaces = "    ";

/// Agrega piece si cabe en kMaxPasteBytes; si no, marca el recorte.
bool append_bounded(SanitizedPaste& out, std::string_view piece) {
    if (out.text.size() + piece.size() > kMaxPasteBytes) {
        out.truncated = true;
        return false;
    }
    out.text += piece;
    return true;
}

/// Una sola línea: fuera los "\n" de los extremos; los de en medio, espacio.
void to_single_line(std::string& text) {
    const std::size_t begin = text.find_first_not_of('\n');
    if (begin == std::string::npos) {
        text.clear();
        return;
    }
    const std::size_t end = text.find_last_not_of('\n');
    text = text.substr(begin, end - begin + 1);
    for (char& c : text) {
        if (c == '\n') {
            c = ' ';
        }
    }
}

} // namespace

SanitizedPaste sanitize_paste(std::string_view text, bool multiline) {
    SanitizedPaste out;
    std::size_t i = 0;
    while (i < text.size()) {
        const std::size_t start = i;
        const std::optional<std::uint32_t> code = utf8::next_code_point(text, i);
        std::string_view piece;
        if (!code.has_value()) {
            ++i; // Cada byte inválido es un U+FFFD.
            piece = utf8::kReplacement;
        } else if (*code == '\r') {
            if (i < text.size() && text[i] == '\n') {
                ++i; // "\r\n" es un solo salto.
            }
            piece = "\n";
        } else if (*code == '\n') {
            piece = "\n";
        } else if (*code == '\t') {
            piece = kTabSpaces;
        } else if (*code < 0x20 || *code == 0x7F || (*code >= 0x80 && *code <= 0x9F)) {
            continue; // Controles C0, DEL y C1.
        } else {
            piece = text.substr(start, i - start);
        }
        if (!append_bounded(out, piece)) {
            break;
        }
    }
    if (!multiline) {
        to_single_line(out.text);
    }
    return out;
}

} // namespace chatbot::cli
