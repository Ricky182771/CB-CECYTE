#include "chatbot/bot/split.h"

#include "chatbot/markdown/source_blocks.h"
#include "chatbot/utf8.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <optional>
#include <utility>

namespace chatbot::bot {
namespace {

bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

bool is_blank(std::string_view text) {
    return std::all_of(text.begin(), text.end(), is_space);
}

/// Sin las líneas en blanco del principio ni los espacios del final: así
/// sale cada pedazo. Vacío si el texto solo tiene espacios.
std::string_view trimmed(std::string_view text) {
    while (!text.empty() && is_space(text.back())) {
        text.remove_suffix(1);
    }
    std::size_t start = 0;
    while (start < text.size()) {
        const std::size_t newline = text.find('\n', start);
        if (newline == std::string_view::npos ||
            !is_blank(text.substr(start, newline - start))) {
            break;
        }
        start = newline + 1;
    }
    return text.substr(start);
}

/// Códigos que no deben quedar al principio de un pedazo porque se pegan al
/// anterior: marcas combinantes, el ZWJ, los selectores de variante, los
/// tonos de piel, el keycap y las etiquetas de las banderas.
bool joins_previous(std::uint32_t code) {
    return (code >= 0x0300 && code <= 0x036F) || code == 0x200D ||
           (code >= 0xFE00 && code <= 0xFE0F) || (code >= 0x1F3FB && code <= 0x1F3FF) ||
           code == 0x20E3 || (code >= 0xE0020 && code <= 0xE007F);
}

/// Código que empieza en text[i], o nullopt si el byte es inválido.
std::optional<std::uint32_t> code_at(std::string_view text, std::size_t i) {
    return i < text.size() ? utf8::next_code_point(text, i) : std::nullopt;
}

/// Mayor candidato c (en orden creciente) con fits(c), o nullopt.
std::optional<std::size_t> largest_fitting(const std::vector<std::size_t>& candidates,
                                           const std::function<bool(std::size_t)>& fits) {
    std::size_t low = 0;
    std::size_t high = candidates.size(); // Busca en [low, high).
    std::optional<std::size_t> best;
    while (low < high) {
        const std::size_t middle = low + (high - low) / 2;
        if (fits(candidates[middle])) {
            best = candidates[middle];
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return best;
}

/// Dónde cortar: el pedazo es text[pos, cut) y el siguiente empieza en next.
struct Cut {
    std::size_t cut = 0;
    std::size_t next = 0;
};

/// El corte más lejano en (pos, end] con fits(corte): primero después de
/// un salto de línea (si lines), luego en un espacio y, si no hay, entre
/// caracteres (sin separar un carácter de sus marcas combinantes cuando se
/// puede). keep_spaces: el espacio se queda al final del pedazo (código);
/// si no, se quita. Siempre avanza: si ni un carácter cabe, corta después
/// del primero (con un tope de kMinMessageLimit nunca pasa).
Cut find_cut(std::string_view text, std::size_t pos, std::size_t end,
             const std::function<bool(std::size_t)>& fits, bool lines, bool keep_spaces) {
    std::vector<std::size_t> candidates;
    if (lines) {
        for (std::size_t i = pos; i < end; ++i) {
            if (text[i] == '\n') {
                candidates.push_back(i + 1);
            }
        }
        if (const std::optional<std::size_t> best = largest_fitting(candidates, fits)) {
            return {*best, *best};
        }
    }
    candidates.clear();
    for (std::size_t i = pos; i < end; ++i) {
        if (text[i] == ' ' && (keep_spaces || i > pos)) {
            candidates.push_back(keep_spaces ? i + 1 : i);
        }
    }
    if (const std::optional<std::size_t> best = largest_fitting(candidates, fits)) {
        std::size_t next = *best;
        while (!keep_spaces && next < end && text[next] == ' ') {
            ++next;
        }
        return {*best, next};
    }
    candidates.clear();
    for (std::size_t i = pos + 1; i <= end; ++i) {
        if (i == text.size() || (static_cast<unsigned char>(text[i]) & 0xC0) != 0x80) {
            candidates.push_back(i);
        }
    }
    std::size_t cut = candidates.empty() ? end : candidates.front();
    if (const std::optional<std::size_t> best = largest_fitting(candidates, fits)) {
        cut = *best;
        // Retrocede mientras el corte separe un carácter de lo que se le pega.
        auto index = static_cast<std::size_t>(
            std::lower_bound(candidates.begin(), candidates.end(), cut) - candidates.begin());
        while (index > 0) {
            // El carácter que termina en el corte empieza en el corte anterior.
            const std::optional<std::uint32_t> before = code_at(text, candidates[index - 1]);
            const std::optional<std::uint32_t> after = code_at(text, candidates[index]);
            const bool glued = (after.has_value() && joins_previous(*after)) ||
                               (before.has_value() && *before == 0x200D);
            if (!glued) {
                break;
            }
            --index;
        }
        cut = candidates[index];
    }
    return {cut, cut};
}

class Splitter {
public:
    explicit Splitter(const MessageLimit& limit)
        : limit_(limit), max_(std::max(limit.max_size, kMinMessageLimit)) {}

    [[nodiscard]] bool fits(std::string_view text) const {
        return limit_.size_of(trimmed(text)) <= max_;
    }

    /// Bytes que puede tener, como mucho, un pedazo que cabe: measure mide
    /// al menos 1 por carácter y un carácter tiene a lo más 4 bytes. Acota
    /// lo que se revisa en cada corte.
    [[nodiscard]] std::size_t window() const { return 4 * max_ + 4; }

    std::vector<std::string> plain(std::string_view text) const {
        std::vector<std::string> pieces;
        std::size_t pos = 0;
        while (pos < text.size()) {
            const std::string_view rest = trimmed(text.substr(pos));
            if (rest.empty()) {
                break;
            }
            if (rest.size() <= window() && fits(rest)) {
                pieces.emplace_back(rest);
                break;
            }
            const std::size_t end = std::min(text.size(), pos + window());
            const Cut cut = find_cut(
                text, pos, end,
                [&](std::size_t c) { return fits(text.substr(pos, c - pos)); }, true, false);
            const std::string_view piece = trimmed(text.substr(pos, cut.cut - pos));
            if (!piece.empty()) {
                pieces.emplace_back(piece);
            }
            pos = cut.next;
        }
        return pieces;
    }

    /// Un bloque de código con cerca que no cabe: varios bloques con la
    /// línea de la cerca original y una cerca que cierra.
    std::vector<std::string> code(std::string_view markdown, const md::SourceBlock& block) const {
        const md::CodeFence& fence = *block.fence;
        std::string open{markdown.substr(fence.open_begin, fence.content_begin - fence.open_begin)};
        if (open.empty() || open.back() != '\n') {
            open += '\n';
        }
        const std::string& close = fence.fence;
        const std::string_view whole = markdown.substr(block.begin, block.end - block.begin);
        if (!fits_exactly(open + "x\n" + close)) {
            return plain(whole); // Un tope absurdo: ni la cerca cabe.
        }

        const std::string_view content =
            markdown.substr(fence.content_begin, fence.content_end - fence.content_begin);
        // Lo que no es del bloque pero cae en su rango (definiciones de
        // enlaces antes de la cerca; líneas en blanco, definiciones o notas
        // después de la que cierra) sale aparte, como texto, para no perderlo.
        std::vector<std::string> pieces =
            plain(markdown.substr(block.begin, fence.open_begin - block.begin));
        std::string lines; // Líneas del pedazo actual, cada una con su '\n'.
        const auto flush = [&] {
            if (!lines.empty()) {
                pieces.push_back(open + lines + close);
                lines.clear();
            }
        };
        std::size_t pos = 0;
        while (pos < content.size()) {
            const std::size_t newline = content.find('\n', pos);
            const std::size_t stop = newline == std::string_view::npos ? content.size() : newline;
            std::string line{content.substr(pos, stop - pos)};
            line += '\n';
            pos = newline == std::string_view::npos ? content.size() : newline + 1;

            if (fits_exactly(open + lines + line + close)) {
                lines += line;
                continue;
            }
            flush();
            if (fits_exactly(open + line + close)) {
                lines = line;
                continue;
            }
            // Una línea que no cabe sola: en pedazos, sin quitar espacios.
            const std::string_view text{line.data(), line.size() - 1};
            std::size_t at = 0;
            while (at < text.size()) {
                const std::size_t end = std::min(text.size(), at + window());
                const Cut cut = find_cut(
                    text, at, end,
                    [&](std::size_t c) {
                        return fits_exactly(open + std::string{text.substr(at, c - at)} + "\n" +
                                            close);
                    },
                    false, true);
                lines = std::string{text.substr(at, cut.cut - at)} + "\n";
                at = cut.next;
                if (at < text.size()) {
                    flush();
                }
            }
        }
        flush();
        const std::string_view after = markdown.substr(fence.close_end, block.end - fence.close_end);
        for (std::string& piece : plain(after)) {
            pieces.push_back(std::move(piece));
        }
        return pieces;
    }

private:
    /// Sin recortar: para los bloques de código, que ya terminan en la cerca.
    [[nodiscard]] bool fits_exactly(const std::string& text) const {
        return limit_.size_of(text) <= max_;
    }

    const MessageLimit& limit_;
    std::size_t max_;
};

/// Espacios del final del texto (lo que separaba el bloque del siguiente).
std::string_view trailing_space(std::string_view text) {
    std::size_t start = text.size();
    while (start > 0 && is_space(text[start - 1])) {
        --start;
    }
    return text.substr(start);
}

} // namespace

std::vector<std::string> split_plain(std::string_view text, const MessageLimit& limit) {
    return Splitter{limit}.plain(text);
}

std::vector<std::string> split_markdown(std::string_view markdown, const MessageLimit& limit) {
    const Splitter splitter{limit};
    const std::vector<md::SourceBlock> blocks = md::source_blocks(markdown);
    if (blocks.empty()) {
        return splitter.plain(markdown); // Sin bloques (definiciones de enlaces sueltas).
    }

    // Unidades que se juntan en pedazos: cada bloque tal cual (los rangos
    // van seguidos, así que juntos reproducen el texto) o, si no cabe solo,
    // sus partes. La última parte lleva los espacios con que terminaba el
    // bloque, para no pegarse al siguiente.
    std::vector<std::string> units;
    for (const md::SourceBlock& block : blocks) {
        const std::string_view text = markdown.substr(block.begin, block.end - block.begin);
        if (splitter.fits(text)) {
            units.emplace_back(text);
            continue;
        }
        std::vector<std::string> parts = block.blocks == 1 && block.fence.has_value()
                                             ? splitter.code(markdown, block)
                                             : splitter.plain(text);
        const std::string_view after = trailing_space(text);
        for (std::size_t i = 0; i < parts.size(); ++i) {
            const bool last = i + 1 == parts.size();
            units.push_back(std::move(parts[i]) +
                            (last && !after.empty() ? std::string{after} : std::string{"\n"}));
        }
    }

    std::vector<std::string> pieces;
    std::string current;
    const auto flush = [&] {
        const std::string_view piece = trimmed(current);
        if (!piece.empty()) {
            pieces.emplace_back(piece);
        }
        current.clear();
    };
    for (std::string& unit : units) {
        if (!current.empty() && !splitter.fits(current + unit)) {
            flush();
        }
        current += unit;
    }
    flush();
    return pieces;
}

} // namespace chatbot::bot
