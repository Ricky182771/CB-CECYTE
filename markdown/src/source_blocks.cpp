#include "chatbot/markdown/source_blocks.h"

#include "parse_spans.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace chatbot::md {
namespace {

/// Inicio de cada línea del texto: 0 y cada posición después de un '\n'
/// (salvo el final del texto, que no empieza una línea).
std::vector<std::size_t> line_starts(std::string_view text) {
    std::vector<std::size_t> starts;
    if (text.empty()) {
        return starts;
    }
    starts.push_back(0);
    for (std::size_t i = 0; i + 1 < text.size(); ++i) {
        if (text[i] == '\n') {
            starts.push_back(i + 1);
        }
    }
    return starts;
}

/// Índice de la línea que contiene el byte offset.
std::size_t line_of(const std::vector<std::size_t>& starts, std::size_t offset) {
    const auto after = std::upper_bound(starts.begin(), starts.end(), offset);
    return static_cast<std::size_t>(after - starts.begin()) - 1;
}

/// Fin de la línea que empieza en begin: después de su '\n', o limit.
std::size_t line_end(std::string_view text, std::size_t begin, std::size_t limit) {
    const std::size_t newline = text.find('\n', begin);
    return newline == std::string_view::npos || newline >= limit ? limit : newline + 1;
}

bool is_blank(std::string_view line) {
    return line.find_first_not_of(" \t\r\n") == std::string_view::npos;
}

/// true si text, parseado solo, da exactamente count bloques de primer
/// nivel con los tipos de expected[first, first + count).
bool same_kinds(std::string_view text, const std::vector<Block>& expected, std::size_t first,
                std::size_t count) {
    const Document document = parse(text);
    if (document.blocks.size() != count) {
        return false;
    }
    for (std::size_t i = 0; i < count; ++i) {
        if (document.blocks[i].kind != expected[first + i].kind) {
            return false;
        }
    }
    return true;
}

/// Cerca de una línea: hasta 3 espacios y luego 3 o más '`' o '~'. Devuelve
/// los caracteres de la cerca, o vacío si la línea no empieza con una.
std::string fence_of(std::string_view line) {
    std::size_t i = 0;
    while (i < line.size() && i < 4 && line[i] == ' ') {
        ++i;
    }
    if (i > 3 || i >= line.size() || (line[i] != '`' && line[i] != '~')) {
        return {};
    }
    const char mark = line[i];
    const std::size_t begin = i;
    while (i < line.size() && line[i] == mark) {
        ++i;
    }
    return i - begin >= 3 ? std::string(i - begin, mark) : std::string{};
}

/// Cerca que cierra la que abrió con open: hasta 3 espacios, al menos tantos
/// caracteres de la misma marca y después solo espacios.
bool closes(std::string_view line, const std::string& open) {
    const std::string fence = fence_of(line);
    if (fence.empty() || fence.front() != open.front() || fence.size() < open.size()) {
        return false;
    }
    const std::size_t after = line.find(fence) + fence.size();
    return is_blank(line.substr(after));
}

/// Las líneas de la cerca de un bloque Code que ocupa [begin, end): la
/// primera línea que es una cerca la abre (antes solo puede haber líneas en
/// blanco o definiciones de enlaces) y la primera cerca que cierra después
/// de ella lo cierra, como en CommonMark (después pueden seguir líneas en
/// blanco, definiciones de enlaces o notas al pie). Nullopt si es código con
/// sangría.
std::optional<CodeFence> code_fence(std::string_view text, std::size_t begin, std::size_t end) {
    std::size_t pos = begin;
    while (pos < end) {
        const std::size_t next = line_end(text, pos, end);
        const std::string fence = fence_of(text.substr(pos, next - pos));
        if (!fence.empty()) {
            CodeFence found;
            found.open_begin = pos;
            found.content_begin = next;
            found.content_end = end;
            found.close_end = end;
            found.fence = fence;
            for (std::size_t line = next; line < end;) {
                const std::size_t stop = line_end(text, line, end);
                if (closes(text.substr(line, stop - line), fence)) {
                    found.content_end = line;
                    found.close_end = stop;
                    break;
                }
                line = stop;
            }
            return found;
        }
        pos = next;
    }
    return std::nullopt;
}

/// Un rango ya revisado: bloques [first, first + count) en [begin, end).
struct Range {
    std::size_t begin = 0;
    std::size_t end = 0;
    std::size_t first = 0;
    std::size_t count = 0;
};

std::vector<Range> find_ranges(std::string_view markdown, const std::vector<Block>& blocks,
                               const std::vector<detail::TextSpan>& spans) {
    const std::size_t n = blocks.size();
    const std::vector<std::size_t> starts = line_starts(markdown);
    const std::size_t lines = starts.size();

    // Siguiente bloque con texto desde cada índice (n si no hay).
    std::vector<std::size_t> next_spanned(n + 1, n);
    for (std::size_t i = n; i-- > 0;) {
        next_spanned[i] = spans[i].empty() ? next_spanned[i + 1] : i;
    }

    std::vector<Range> ranges;
    Range current;
    std::optional<std::size_t> last_text_line; // Del rango actual.
    for (std::size_t k = 1; k < n; ++k) {
        // Frontera entre el bloque k - 1 y el k: el inicio de una línea L,
        // después de la última línea con texto de los bloques del rango y
        // antes de la primera del siguiente bloque con texto, dejando una
        // línea para cada bloque sin texto de en medio.
        const detail::TextSpan& previous = spans[k - 1];
        if (!previous.empty()) {
            last_text_line = line_of(starts, previous.end - 1);
        }
        std::size_t lower = line_of(starts, current.begin) + 1;
        if (last_text_line.has_value()) {
            lower = std::max(lower, *last_text_line + 1);
        }
        const std::size_t spanned = next_spanned[k];
        const std::size_t limit = spanned < n ? line_of(starts, spans[spanned].begin) : lines;
        const std::size_t textless = spanned - k;
        if (limit >= textless) {
            const std::size_t upper = limit - textless;
            // De la última a la primera: las líneas en blanco y las que
            // cierran un bloque (cerca, subrayado de setext) se quedan con
            // él. Una línea que abre el siguiente bloque (su cerca, un
            // elemento vacío de lista) agregaría un bloque al rango, y la
            // revisión la rechaza.
            for (std::size_t line = upper + 1; line-- > lower;) {
                const std::size_t at = starts[line];
                if (same_kinds(markdown.substr(current.begin, at - current.begin), blocks,
                               current.first, k - current.first)) {
                    current.end = at;
                    current.count = k - current.first;
                    ranges.push_back(current);
                    current = Range{at, 0, k, 0};
                    last_text_line.reset();
                    break;
                }
            }
        }
        // Si ninguna línea pasó la revisión, el bloque k sigue en este rango.
    }
    current.end = markdown.size();
    current.count = n - current.first;
    ranges.push_back(current);

    // El último rango no pasó por la revisión: si no da sus bloques, se
    // junta con el anterior (el documento completo siempre los da).
    while (ranges.size() > 1) {
        const Range& last = ranges.back();
        if (same_kinds(markdown.substr(last.begin, last.end - last.begin), blocks, last.first,
                       last.count)) {
            break;
        }
        Range merged = ranges[ranges.size() - 2];
        merged.end = last.end;
        merged.count += last.count;
        ranges.pop_back();
        ranges.back() = merged;
    }
    return ranges;
}

} // namespace

std::vector<SourceBlock> source_blocks(std::string_view markdown) {
    try {
        const detail::ParsedSpans parsed = detail::parse_with_spans(markdown);
        const std::vector<Block>& blocks = parsed.document.blocks;
        if (blocks.empty()) {
            return {};
        }
        std::vector<Range> ranges;
        if (parsed.spans.size() == blocks.size()) {
            ranges = find_ranges(markdown, blocks, parsed.spans);
        } else {
            // md4c no lo parseó (parse() usó el párrafo de respaldo).
            ranges.push_back(Range{0, markdown.size(), 0, blocks.size()});
        }
        std::vector<SourceBlock> out;
        out.reserve(ranges.size());
        for (const Range& range : ranges) {
            SourceBlock block;
            block.kind = blocks[range.first].kind;
            block.begin = range.begin;
            block.end = range.end;
            block.blocks = range.count;
            if (block.kind == Block::Kind::Code && block.blocks == 1) {
                block.fence = code_fence(markdown, range.begin, range.end);
            }
            out.push_back(std::move(block));
        }
        return out;
    } catch (...) {
        return {};
    }
}

} // namespace chatbot::md
