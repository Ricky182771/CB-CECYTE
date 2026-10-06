#include "system_prompt.h"

#include <cstdint>

namespace chatbot::cli {
namespace {

/// Decodifica el código en text[i] y avanza i. Nullopt si la secuencia no es
/// UTF-8 válido (incompleta, sobrelarga, sustituto o mayor que U+10FFFF).
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

/// Línea (desde 1) en la que está el byte offset.
std::size_t line_of(std::string_view text, std::size_t offset) {
    std::size_t line = 1;
    for (std::size_t k = 0; k < offset; ++k) {
        if (text[k] == '\n') {
            ++line;
        }
    }
    return line;
}

} // namespace

ResolvedSystemPrompt resolve_system_prompt(const Result<std::optional<std::string>>& loaded) {
    if (loaded.is_error()) {
        return {std::string{kDefaultSystemPrompt},
                loaded.error().message + " Se usan las instrucciones del sistema predeterminadas."};
    }
    if (!loaded.value().has_value()) {
        return {std::string{kDefaultSystemPrompt}, {}};
    }
    return {*loaded.value(), {}};
}

std::optional<std::string> validate_system_prompt(std::string_view text) {
    if (text.size() > kMaxSystemPromptBytes) {
        return "Las instrucciones miden " + std::to_string(text.size()) +
               " bytes; el máximo es " + std::to_string(kMaxSystemPromptBytes) + ".";
    }
    std::size_t i = 0;
    while (i < text.size()) {
        const std::size_t start = i;
        const std::optional<std::uint32_t> code = next_code_point(text, i);
        if (!code.has_value()) {
            return "Las instrucciones no son UTF-8 válido (línea " +
                   std::to_string(line_of(text, start)) + ").";
        }
        const bool c0 = *code < 0x20 && *code != '\n' && *code != '\t';
        const bool del_or_c1 = *code >= 0x7F && *code <= 0x9F;
        if (c0 || del_or_c1) {
            return "Las instrucciones tienen un carácter de control no permitido en la línea " +
                   std::to_string(line_of(text, start)) +
                   " (solo se aceptan saltos de línea y tabuladores).";
        }
    }
    return std::nullopt;
}

std::optional<std::string> system_prompt_limit_warning(std::string_view text,
                                                       std::size_t history_limit_bytes) {
    if (history_limit_bytes == 0 || text.size() < history_limit_bytes) {
        return std::nullopt;
    }
    return "Las instrucciones miden " + std::to_string(text.size()) +
           " bytes y el límite del historial es " + std::to_string(history_limit_bytes) +
           ": el recorte nunca las quita, así que queda poco o nada de espacio para la "
           "conversación.";
}

std::optional<std::string> system_prompt_to_store(std::string_view text) {
    if (text == kDefaultSystemPrompt) {
        return std::nullopt;
    }
    return std::string{text};
}

} // namespace chatbot::cli
