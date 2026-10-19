#include "chatbot/telegram/rich_markdown.h"

#include "chatbot/markdown/markdown.h"
#include "chatbot/markdown/source_blocks.h"
#include "chatbot/web_search.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace chatbot::telegram {
namespace {

using md::Block;
using md::Run;

/// md::parse como lo necesita el saneado: el código tal cual y los saltos
/// de línea del texto. md4c entrega la sangría de cada línea de código como
/// espacios (un tabulador al principio llega como cuatro): en los bloques
/// con cerca de primer nivel sin sangría, el contenido se toma del texto
/// original, byte por byte.
md::Document parse_for_telegram(std::string_view markdown) {
    md::ParseOptions options;
    options.raw_code = true;
    options.keep_soft_breaks = true;
    md::Document document = md::parse(markdown, options);
    std::size_t index = 0;
    for (const md::SourceBlock& source : md::source_blocks(markdown)) {
        if (source.fence.has_value() && index < document.blocks.size() &&
            document.blocks[index].kind == Block::Kind::Code) {
            const md::CodeFence& fence = *source.fence;
            const char first = markdown[fence.open_begin];
            if (first == '`' || first == '~') { // La cerca sin sangría.
                document.blocks[index].code = std::string{
                    markdown.substr(fence.content_begin, fence.content_end - fence.content_begin)};
            }
        }
        index += source.blocks;
    }
    return document;
}

bool is_space(char c) {
    return c == ' ' || c == '\t';
}

/// El tramo de backticks más largo del texto.
std::size_t longest_run(std::string_view text, char mark) {
    std::size_t longest = 0;
    std::size_t current = 0;
    for (const char c : text) {
        current = c == mark ? current + 1 : 0;
        longest = std::max(longest, current);
    }
    return longest;
}

/// Prefija cada línea: first en la primera, rest en las demás. Una línea
/// vacía lleva rest sin los espacios del final (">" en una cita).
std::string prefix_lines(std::string_view text, std::string_view first, std::string_view rest) {
    std::string blank{rest};
    while (!blank.empty() && blank.back() == ' ') {
        blank.pop_back();
    }
    std::string out;
    std::size_t pos = 0;
    bool first_line = true;
    while (true) {
        const std::size_t newline = text.find('\n', pos);
        const std::string_view line =
            text.substr(pos, newline == std::string_view::npos ? std::string_view::npos
                                                               : newline - pos);
        if (first_line) {
            out += first;
            out += line;
        } else if (line.empty()) {
            out += blank;
        } else {
            out += rest;
            out += line;
        }
        if (newline == std::string_view::npos) {
            break;
        }
        out += '\n';
        pos = newline + 1;
        first_line = false;
    }
    return out;
}

/// URL de un enlace: sin espacios, paréntesis, < > ni \ (que cortarían el
/// destino o abrirían una etiqueta), con %XX.
std::string link_destination(std::string_view url) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    for (const char c : url) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte <= 0x20 || byte == 0x7F || c == '(' || c == ')' || c == '<' || c == '>' ||
            c == '\\' || c == '`' || c == '"') {
            out += '%';
            out += kHex[byte >> 4U];
            out += kHex[byte & 0x0FU];
        } else {
            out += c;
        }
    }
    return out;
}

/// Lenguaje de una cerca: solo letras, números y + # . _ - (lo demás se
/// quita para que nada raro quede en la línea de la cerca).
std::string fence_language(std::string_view info) {
    std::string out;
    for (const char c : info) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '+' || c == '#' || c == '.' || c == '_' || c == '-') {
            out += c;
        } else {
            break;
        }
    }
    return out;
}

const char* alert_title(const std::string& type) {
    if (type == "tip") {
        return "Consejo";
    }
    if (type == "important") {
        return "Importante";
    }
    if (type == "warning") {
        return "Advertencia";
    }
    if (type == "caution") {
        return "Precaución";
    }
    return "Nota";
}

/// Escribe el contenido en línea de un bloque (párrafo, encabezado, celda).
class InlineWriter {
public:
    /// single_line: los saltos de línea pasan a espacios (encabezados,
    /// celdas).
    InlineWriter(const Block& block, bool single_line) : block_(block), single_line_(single_line) {}

    std::string write() {
        std::size_t i = 0;
        const std::vector<Run>& runs = block_.runs;
        while (i < runs.size()) {
            const int link = runs[i].link;
            std::size_t end = i;
            while (end < runs.size() && runs[end].link == link) {
                ++end;
            }
            if (link >= 0 && static_cast<std::size_t>(link) < block_.links.size()) {
                write_link(block_.links[static_cast<std::size_t>(link)], i, end);
            } else {
                write_runs(i, end, false);
            }
            i = end;
        }
        close_styles();
        out_ += pending_space_;
        pending_space_.clear();
        return out_;
    }

private:
    static constexpr std::uint8_t kStyles[] = {md::kStrong, md::kEmphasis, md::kStrike,
                                               md::kHighlight};

    static const char* marker(std::uint8_t style) {
        switch (style) {
        case md::kStrong:
            return "**";
        case md::kEmphasis:
            return "*";
        case md::kStrike:
            return "~~";
        default:
            return "==";
        }
    }

    void write_link(const md::Link& link, std::size_t begin, std::size_t end) {
        if (!is_allowed_link(link.url)) {
            write_runs(begin, end, true); // Solo el texto.
            return;
        }
        close_styles();
        out_ += pending_space_;
        pending_space_.clear();
        out_ += '[';
        line_start_ = false;
        const std::size_t before = out_.size();
        write_runs(begin, end, true);
        close_styles();
        out_ += pending_space_;
        pending_space_.clear();
        if (out_.size() == before) {
            escape(link.url); // Un enlace sin texto muestra la URL.
        }
        out_ += "](" + link_destination(link.url) + ")";
    }

    /// in_link: dentro del texto de un enlace no puede ir otro enlace (una
    /// imagen queda como su texto).
    void write_runs(std::size_t begin, std::size_t end, bool in_link) {
        for (std::size_t i = begin; i < end; ++i) {
            const Run& run = block_.runs[i];
            switch (run.kind) {
            case Run::Kind::Text:
                write_text(run.text, run.style);
                break;
            case Run::Kind::Code:
                write_atom(code_span(run.text), run.style);
                break;
            case Run::Kind::Math:
                if (const std::string latex = math(run.text); !latex.empty()) {
                    write_atom("$" + latex + "$", run.style);
                }
                break;
            case Run::Kind::MathDisplay:
                if (const std::string latex = math(run.text); !latex.empty()) {
                    write_atom("$$" + latex + "$$", run.style);
                }
                break;
            case Run::Kind::LineBreak:
                close_styles();
                pending_space_.clear();
                out_ += single_line_ ? " " : "\\\n";
                line_start_ = !single_line_;
                break;
            case Run::Kind::Image:
                write_image(run, in_link);
                break;
            case Run::Kind::FootnoteRef:
                write_atom("[^" + std::to_string(run.footnote) + "]", run.style);
                break;
            }
        }
    }

    /// Una imagen o un medio: un enlace normal con su texto alternativo (o
    /// la URL), nunca un bloque de medios. Dentro de un enlace, o con una
    /// URL que no se permite, solo el texto.
    void write_image(const Run& run, bool in_link) {
        if (in_link || !is_allowed_link(run.url)) {
            write_text(run.text, run.style);
            return;
        }
        close_styles();
        out_ += pending_space_;
        pending_space_.clear();
        out_ += '[';
        line_start_ = false;
        const std::size_t before = out_.size();
        write_text(run.text, run.style);
        close_styles();
        pending_space_.clear();
        if (out_.size() == before) {
            escape(run.url);
        }
        out_ += "](" + link_destination(run.url) + ")";
    }

    /// Algo que no se escapa (código, fórmula, referencia), con los estilos
    /// del tramo.
    void write_atom(const std::string& atom, std::uint8_t style) {
        set_styles(style);
        out_ += atom;
        line_start_ = false;
    }

    void write_text(std::string_view text, std::uint8_t style) {
        // Los espacios de las orillas quedan fuera de las marcas de estilo:
        // "**negritas **" no se entendería.
        std::size_t lead = 0;
        while (lead < text.size() && (is_space(text[lead]) || text[lead] == '\n')) {
            ++lead;
        }
        if (lead == text.size()) {
            add_space(text);
            return;
        }
        std::size_t trail = text.size();
        while (trail > lead && (is_space(text[trail - 1]) || text[trail - 1] == '\n')) {
            --trail;
        }
        const std::string_view core = text.substr(lead, trail - lead);
        if (style != open_mask()) {
            // Se cierran antes del espacio y se abren después.
            close_unless(style);
        }
        add_space(text.substr(0, lead));
        out_ += pending_space_;
        pending_space_.clear();
        open_missing(style);
        escape(core);
        add_space(text.substr(trail));
    }

    /// Espacios entre tramos: se escriben antes del siguiente texto. Un
    /// salto de línea fuera de un encabezado o celda se conserva.
    void add_space(std::string_view space) {
        for (const char c : space) {
            if (c == '\n' && !single_line_) {
                close_styles();
                pending_space_.clear();
                out_ += '\n';
                line_start_ = true;
            } else if (!line_start_) {
                pending_space_ += c == '\n' ? ' ' : c;
            }
        }
    }

    void set_styles(std::uint8_t style) {
        close_unless(style);
        out_ += pending_space_;
        pending_space_.clear();
        open_missing(style);
    }

    [[nodiscard]] std::uint8_t open_mask() const {
        std::uint8_t mask = 0;
        for (const std::uint8_t style : open_) {
            mask = static_cast<std::uint8_t>(mask | style);
        }
        return mask;
    }

    /// Cierra, desde el de adentro, hasta que todo lo abierto esté en style.
    void close_unless(std::uint8_t style) {
        std::size_t keep = 0;
        while (keep < open_.size() && (open_[keep] & style) != 0) {
            ++keep;
        }
        while (open_.size() > keep) {
            out_ += marker(open_.back());
            open_.pop_back();
        }
    }

    void open_missing(std::uint8_t style) {
        for (const std::uint8_t candidate : kStyles) {
            if ((style & candidate) != 0 && (open_mask() & candidate) == 0) {
                out_ += marker(candidate);
                open_.push_back(candidate);
            }
        }
    }

    void close_styles() { close_unless(0); }

    /// Texto escapado: todo lo que Telegram podría entender como formato,
    /// etiqueta, entidad o medio va con '\'. Al principio de una línea,
    /// también lo que abriría un bloque (#, >, -, +, "1.", "1)").
    void escape(std::string_view text) {
        for (std::size_t i = 0; i < text.size(); ++i) {
            const char c = text[i];
            if (c == '\n') {
                if (single_line_) {
                    out_ += ' ';
                } else {
                    out_ += '\n';
                    line_start_ = true;
                }
                continue;
            }
            if (line_start_) {
                if (is_space(c)) {
                    continue; // Con sangría sería código.
                }
                if (c == '#' || c == '>' || c == '-' || c == '+' || c == '=') {
                    out_ += '\\';
                    out_ += c;
                    line_start_ = false;
                    continue;
                }
                if (c >= '0' && c <= '9') {
                    std::size_t digits = i;
                    while (digits < text.size() && text[digits] >= '0' && text[digits] <= '9') {
                        ++digits;
                    }
                    if (digits < text.size() && (text[digits] == '.' || text[digits] == ')')) {
                        out_.append(text.substr(i, digits - i));
                        out_ += '\\';
                        out_ += text[digits];
                        i = digits;
                        line_start_ = false;
                        continue;
                    }
                }
                line_start_ = false;
            }
            switch (c) {
            case '\\':
            case '`':
            case '*':
            case '_':
            case '~':
            case '[':
            case ']':
            case '<':
            case '>':
            case '|':
            case '$':
            case '&':
                out_ += '\\';
                out_ += c;
                break;
            case '!':
                // "![" sería una imagen si lo que sigue es un enlace.
                if (i + 1 == text.size()) {
                    out_ += '\\';
                }
                out_ += c;
                break;
            case '=': {
                const bool doubled = (i > 0 && text[i - 1] == '=') ||
                                     (i + 1 < text.size() && text[i + 1] == '=');
                if (doubled) {
                    out_ += '\\';
                }
                out_ += c;
                break;
            }
            default:
                out_ += c;
                break;
            }
        }
    }

    /// Fragmento de código con una cerca más larga que cualquier tramo de
    /// backticks del contenido; el contenido no cambia.
    static std::string code_span(std::string_view text) {
        std::string content{text};
        std::replace(content.begin(), content.end(), '\n', ' ');
        const std::string fence(longest_run(content, '`') + 1, '`');
        const bool pad = !content.empty() && (content.front() == '`' || content.back() == '`' ||
                                              (content.front() == ' ' && content.back() == ' ' &&
                                               content.find_first_not_of(' ') != std::string::npos));
        return pad ? fence + " " + content + " " + fence : fence + content + fence;
    }

    /// LaTeX de una fórmula en una línea. Por si Telegram no la reconociera
    /// como fórmula, que no quede ni una etiqueta ni un enlace: < y > pasan a
    /// \lt y \gt, y "](", "][" y "![" llevan un espacio en medio (en LaTeX
    /// no cambia nada).
    static std::string math(std::string_view text) {
        std::string out;
        for (const char c : text) {
            if ((c == '(' || c == '[') && !out.empty() && out.back() == ']') {
                out += ' ';
            } else if (c == '[' && !out.empty() && out.back() == '!') {
                out += ' ';
            }
            if (c == '<') {
                out += "\\lt ";
            } else if (c == '>') {
                out += "\\gt ";
            } else if (c == '\n') {
                out += ' ';
            } else {
                out += c;
            }
        }
        const std::size_t first = out.find_first_not_of(' ');
        if (first == std::string::npos) {
            return {};
        }
        const std::size_t last = out.find_last_not_of(' ');
        return out.substr(first, last - first + 1);
    }

    const Block& block_;
    bool single_line_;
    std::string out_;
    std::string pending_space_;
    std::vector<std::uint8_t> open_;
    bool line_start_ = true;
};

std::string write_inline(const Block& block, bool single_line) {
    return InlineWriter{block, single_line}.write();
}

std::string write_blocks(const std::vector<Block>& blocks);
std::string write_block(const Block& block);

std::string write_code(const Block& block) {
    std::string code = block.code;
    if (!code.empty() && code.back() != '\n') {
        code += '\n';
    }
    const std::string fence(std::max<std::size_t>(3, longest_run(code, '`') + 1), '`');
    return fence + fence_language(block.info) + "\n" + code + fence;
}

std::string write_list(const Block& list) {
    std::string out;
    unsigned number = list.start;
    for (const Block& item : list.children) {
        std::string marker = list.kind == Block::Kind::OrderedList
                                 ? std::to_string(number++) + ". "
                                 : std::string{"- "};
        const std::string indent(marker.size(), ' ');
        if (item.task) {
            marker += item.checked ? "[x] " : "[ ] ";
        }
        // Dentro de un elemento, los bloques van seguidos (lista compacta),
        // salvo dos párrafos, que necesitan una línea en blanco.
        std::string content;
        for (std::size_t i = 0; i < item.children.size(); ++i) {
            const Block& child = item.children[i];
            if (i > 0) {
                const Block& previous = item.children[i - 1];
                const bool loose = previous.kind == Block::Kind::Paragraph &&
                                   child.kind == Block::Kind::Paragraph;
                content += loose || child.kind == Block::Kind::Rule ? "\n\n" : "\n";
            }
            content += write_block(child);
        }
        if (!out.empty()) {
            out += '\n';
        }
        out += content.empty() ? std::string{marker.substr(0, marker.find_last_not_of(' ') + 1)}
                               : prefix_lines(content, marker, indent);
    }
    return out;
}

std::string write_table(const Block& table) {
    const std::size_t columns = table.align.size();
    if (columns == 0 || table.rows.empty()) {
        return {};
    }
    const auto row_text = [&](const std::vector<Block>& cells) {
        std::string line = "|";
        for (std::size_t c = 0; c < columns; ++c) {
            const std::string cell = c < cells.size() ? write_inline(cells[c], true) : std::string{};
            line += " " + cell + " |";
        }
        return line;
    };
    std::string out = row_text(table.rows.front()) + "\n|";
    for (const md::Align align : table.align) {
        switch (align) {
        case md::Align::Left:
            out += ":---|";
            break;
        case md::Align::Center:
            out += ":---:|";
            break;
        case md::Align::Right:
            out += "---:|";
            break;
        case md::Align::Default:
            out += "---|";
            break;
        }
    }
    for (std::size_t r = 1; r < table.rows.size(); ++r) {
        out += "\n" + row_text(table.rows[r]);
    }
    return out;
}

std::string write_block(const Block& block) {
    switch (block.kind) {
    case Block::Kind::Paragraph:
        return write_inline(block, false);
    case Block::Kind::Heading:
        return std::string(std::clamp(block.level, 1U, 6U), '#') + " " + write_inline(block, true);
    case Block::Kind::Quote:
        return prefix_lines(write_blocks(block.children), "> ", "> ");
    case Block::Kind::Alert: {
        const std::string body = write_blocks(block.children);
        const std::string title = std::string{"**"} + alert_title(block.info) + "**";
        return prefix_lines(body.empty() ? title : title + "\n\n" + body, "> ", "> ");
    }
    case Block::Kind::BulletList:
    case Block::Kind::OrderedList:
        return write_list(block);
    case Block::Kind::ListItem:
        return write_blocks(block.children);
    case Block::Kind::Code:
        return write_code(block);
    case Block::Kind::Rule:
        return "***";
    case Block::Kind::Table:
        return write_table(block);
    case Block::Kind::FootnoteDef: {
        const std::string body = write_blocks(block.children);
        return prefix_lines(body, "[^" + std::to_string(block.footnote) + "]: ", "    ");
    }
    case Block::Kind::Document:
        return write_blocks(block.children);
    }
    return {};
}

std::string write_blocks(const std::vector<Block>& blocks) {
    std::string out;
    for (const Block& block : blocks) {
        std::string text = write_block(block);
        if (text.find_first_not_of(" \n") == std::string::npos) {
            continue;
        }
        if (!out.empty()) {
            out += "\n\n";
        }
        out += text;
    }
    return out;
}

std::size_t count_blocks(const Block& block) {
    std::size_t count = 1;
    for (const Block& child : block.children) {
        count += count_blocks(child);
    }
    count += block.rows.size(); // Cada fila de una tabla.
    for (const Run& run : block.runs) {
        if (run.kind == Run::Kind::MathDisplay) {
            ++count; // Puede quedar como bloque aparte.
        }
    }
    return count;
}

} // namespace

bool is_allowed_link(std::string_view url) {
    if (is_web_url(url)) {
        return true;
    }
    constexpr std::string_view kMailto = "mailto:";
    if (url.size() <= kMailto.size()) {
        return false;
    }
    for (std::size_t i = 0; i < kMailto.size(); ++i) {
        char c = url[i];
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
        if (c != kMailto[i]) {
            return false;
        }
    }
    return true;
}

std::string to_rich_markdown(std::string_view markdown) {
    try {
        const md::Document document = parse_for_telegram(markdown);
        std::string out = write_blocks(document.blocks);
        const std::string notes = write_blocks(document.footnotes);
        if (!notes.empty()) {
            out += out.empty() ? notes : "\n\n" + notes;
        }
        return out;
    } catch (...) {
        return {}; // Sin memoria: un mensaje vacío, que Telegram rechaza.
    }
}

std::size_t rich_block_count(std::string_view markdown) {
    try {
        const md::Document document = parse_for_telegram(markdown);
        std::size_t count = 0;
        for (const Block& block : document.blocks) {
            count += count_blocks(block);
        }
        for (const Block& block : document.footnotes) {
            count += count_blocks(block);
        }
        return count;
    } catch (...) {
        return kRichMaxBlocks + 1; // Sin saber: que no quepa.
    }
}

bot::MessageLimit rich_message_limit() {
    bot::MessageLimit limit;
    limit.max_size = kRichMaxBytes;
    limit.measure = [](std::string_view text) { return to_rich_markdown(text).size(); };
    limit.max_blocks = kRichMaxBlocks;
    limit.count_blocks = rich_block_count;
    return limit;
}

bot::MessageLimit plain_message_limit() {
    bot::MessageLimit limit;
    limit.max_size = kPlainMaxUnits;
    limit.measure = bot::utf16_length;
    return limit;
}

} // namespace chatbot::telegram
