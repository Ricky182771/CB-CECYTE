#include "markdown_view.h"

#include <ftxui/screen/color.hpp>
#include <ftxui/screen/string.hpp>

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace chatbot::cli::md {
namespace {

using ftxui::Element;
using ftxui::Elements;

/// Aspecto de un fragmento de texto en pantalla.
struct Look {
    bool bold = false;
    bool italic = false;
    bool underline = false;
    bool strike = false;
    bool dim = false;
    bool highlight = false;
    bool code = false;
    std::string link;

    bool operator==(const Look&) const = default;
};

Look merge(Look look, const Look& base) {
    look.bold = look.bold || base.bold;
    look.italic = look.italic || base.italic;
    look.underline = look.underline || base.underline;
    look.strike = look.strike || base.strike;
    look.dim = look.dim || base.dim;
    look.highlight = look.highlight || base.highlight;
    look.code = look.code || base.code;
    if (look.link.empty()) {
        look.link = base.link;
    }
    return look;
}

/// Unidad del flujo en línea: una palabra, un espacio partible, un salto
/// duro, o un bloque que no se parte si cabe (código en línea, LaTeX).
struct Atom {
    enum class Kind { Word, Space, Break };
    Kind kind = Kind::Word;
    std::string text;
    Look look;
};

struct Segment {
    std::string text;
    Look look;
};

/// Una línea ya ajustada al ancho.
struct Line {
    std::vector<Segment> segments;
    int width = 0;

    void append(const std::string& text, int text_width, const Look& look) {
        if (!segments.empty() && segments.back().look == look) {
            segments.back().text += text;
        } else {
            segments.push_back(Segment{text, look});
        }
        width += text_width;
    }
    void append(const std::string& text, const Look& look) {
        append(text, ftxui::string_width(text), look);
    }
};

Look look_of(const Run& run, const Block& block) {
    Look look;
    look.bold = (run.style & kStrong) != 0;
    look.italic = (run.style & kEmphasis) != 0;
    look.strike = (run.style & kStrike) != 0;
    look.highlight = (run.style & kHighlight) != 0;
    if (run.link >= 0 && static_cast<std::size_t>(run.link) < block.links.size()) {
        look.underline = true;
        look.link = block.links[static_cast<std::size_t>(run.link)].url;
    }
    return look;
}

/// Parte texto en palabras y espacios (los '\n' son saltos duros). Con
/// collapse, varios espacios seguidos cuentan como uno (como en markdown);
/// sin él se conservan (texto plano).
void push_words(std::vector<Atom>& atoms, const std::string& text, const Look& look,
                bool collapse = true) {
    std::string word;
    const auto flush = [&] {
        if (!word.empty()) {
            atoms.push_back(Atom{Atom::Kind::Word, std::move(word), look});
            word.clear();
        }
    };
    for (const char c : text) {
        if (c == ' ') {
            flush();
            if (atoms.empty() || atoms.back().kind != Atom::Kind::Space) {
                atoms.push_back(Atom{Atom::Kind::Space, " ", look});
            } else if (!collapse) {
                atoms.back().text.push_back(' ');
            }
        } else if (c == '\n') {
            flush();
            atoms.push_back(Atom{Atom::Kind::Break, {}, look});
        } else {
            word.push_back(c);
        }
    }
    flush();
}

void push_space(std::vector<Atom>& atoms, const Look& look) {
    atoms.push_back(Atom{Atom::Kind::Space, " ", look});
}

/// Contenido en línea de un bloque como átomos.
std::vector<Atom> atoms_of(const Block& block, const Look& base) {
    std::vector<Atom> atoms;
    std::string link_text; // Texto visible del enlace en curso.
    const auto close_link = [&](int index) {
        if (index < 0 || static_cast<std::size_t>(index) >= block.links.size()) {
            return;
        }
        const Link& link = block.links[static_cast<std::size_t>(index)];
        if (!link.autolink && !link.url.empty() && link_text != link.url) {
            Look look = base;
            look.dim = true;
            push_space(atoms, look);
            look.link = link.url;
            push_words(atoms, "(" + link.url + ")", look);
        }
        link_text.clear();
    };
    int open_link = -1;
    for (const Run& run : block.runs) {
        if (run.link != open_link) {
            close_link(open_link);
            open_link = run.link;
        }
        const Look look = merge(look_of(run, block), base);
        if (run.link >= 0) {
            link_text += run.text;
        }
        switch (run.kind) {
        case Run::Kind::Text:
            push_words(atoms, run.text, look);
            break;
        case Run::Kind::Code:
        case Run::Kind::Math:
        case Run::Kind::MathDisplay: {
            Look code = look;
            code.code = true;
            std::string text = run.text;
            std::replace(text.begin(), text.end(), '\n', ' ');
            if (!text.empty()) {
                atoms.push_back(Atom{Atom::Kind::Word, std::move(text), code});
            }
            break;
        }
        case Run::Kind::LineBreak:
            atoms.push_back(Atom{Atom::Kind::Break, {}, look});
            break;
        case Run::Kind::Image: {
            push_words(atoms, run.text.empty() ? "[imagen]" : "[imagen: " + run.text + "]", look);
            if (!run.url.empty()) {
                Look dim = base;
                dim.dim = true;
                push_space(atoms, dim);
                dim.link = run.url;
                push_words(atoms, "(" + run.url + ")", dim);
            }
            break;
        }
        case Run::Kind::FootnoteRef:
            atoms.push_back(
                Atom{Atom::Kind::Word, "[" + std::to_string(run.footnote) + "]", look});
            break;
        }
    }
    close_link(open_link);
    return atoms;
}

/// Ajusta los átomos a width columnas. Una palabra que no cabe en la línea
/// pasa a la siguiente; si no cabe ni en una línea vacía, se parte por
/// caracteres. Toda línea tiene al menos un carácter, aunque sea ancho.
std::vector<Line> flow(const std::vector<Atom>& atoms, int width) {
    width = std::max(width, 1);
    std::vector<Line> lines(1);
    const Atom* pending_space = nullptr;
    for (const Atom& atom : atoms) {
        Line* line = &lines.back();
        switch (atom.kind) {
        case Atom::Kind::Break:
            lines.emplace_back();
            pending_space = nullptr;
            continue;
        case Atom::Kind::Space:
            if (line->width > 0) {
                pending_space = &atom;
            }
            continue;
        case Atom::Kind::Word:
            break;
        }
        const int atom_width = ftxui::string_width(atom.text);
        const int space = pending_space != nullptr ? ftxui::string_width(pending_space->text) : 0;
        if (line->width + space + atom_width <= width) {
            if (pending_space != nullptr) {
                line->append(pending_space->text, space, pending_space->look);
            }
            line->append(atom.text, atom_width, atom.look);
        } else if (atom_width <= width) {
            lines.emplace_back();
            lines.back().append(atom.text, atom_width, atom.look);
        } else {
            // Más ancho que la línea: llena lo que queda y sigue partiendo.
            if (pending_space != nullptr && line->width + space < width) {
                line->append(pending_space->text, space, pending_space->look);
            } else if (line->width > 0) {
                lines.emplace_back();
            }
            for (const std::string& glyph : ftxui::Utf8ToGlyphs(atom.text)) {
                if (glyph.empty()) {
                    continue; // Segunda celda de un carácter ancho.
                }
                const int glyph_width = ftxui::string_width(glyph);
                if (lines.back().width + glyph_width > width && lines.back().width > 0) {
                    lines.emplace_back();
                }
                lines.back().append(glyph, glyph_width, atom.look);
            }
        }
        pending_space = nullptr;
    }
    return lines;
}

Element decorate(Element element, const Look& look) {
    if (look.bold) {
        element = ftxui::bold(std::move(element));
    }
    if (look.italic) {
        element = ftxui::italic(std::move(element));
    }
    if (look.underline) {
        element = ftxui::underlined(std::move(element));
    }
    if (look.strike) {
        element = ftxui::strikethrough(std::move(element));
    }
    if (look.dim) {
        element = ftxui::dim(std::move(element));
    }
    if (look.code) {
        element = ftxui::color(ftxui::Color::Cyan, std::move(element));
    }
    if (look.highlight) {
        // Texto negro sobre amarillo: se lee en temas claros y oscuros.
        element = ftxui::bgcolor(ftxui::Color::Yellow,
                                 ftxui::color(ftxui::Color::Black, std::move(element)));
    }
    if (!look.link.empty()) {
        element = ftxui::hyperlink(hyperlink_target(look.link), std::move(element));
    }
    return element;
}

Element line_element(const Line& line) {
    if (line.segments.empty()) {
        return ftxui::text("");
    }
    if (line.segments.size() == 1) {
        return decorate(ftxui::text(line.segments.front().text), line.segments.front().look);
    }
    Elements parts;
    parts.reserve(line.segments.size());
    for (const Segment& segment : line.segments) {
        parts.push_back(decorate(ftxui::text(segment.text), segment.look));
    }
    return ftxui::hbox(std::move(parts));
}

Element lines_element(const std::vector<Line>& lines) {
    Elements rows;
    rows.reserve(lines.size());
    for (const Line& line : lines) {
        rows.push_back(line_element(line));
    }
    return ftxui::vbox(std::move(rows));
}

/// Agrega espacios a la línea (relleno de celdas de tabla).
void pad(Line& line, int count) {
    if (count > 0) {
        line.append(std::string(static_cast<std::size_t>(count), ' '), count, Look{});
    }
}

std::string repeat(std::string_view piece, int count) {
    std::string out;
    for (int i = 0; i < count; ++i) {
        out += piece;
    }
    return out;
}

std::string alert_label(const std::string& type) {
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

class Renderer {
public:
    Element blocks(const std::vector<Block>& list, int width, bool spaced) {
        Elements rows;
        for (const Block& block : list) {
            if (spaced && !rows.empty()) {
                rows.push_back(ftxui::text(""));
            }
            rows.push_back(block_element(block, width));
        }
        if (rows.empty()) {
            return ftxui::text("");
        }
        return ftxui::vbox(std::move(rows));
    }

private:
    Element block_element(const Block& block, int width) {
        width = std::max(width, 1);
        switch (block.kind) {
        case Block::Kind::Heading: {
            Look base;
            base.bold = true;
            base.underline = block.level == 1;
            base.dim = block.level >= 3;
            return lines_element(flow(atoms_of(block, base), width));
        }
        case Block::Kind::Quote:
            return with_bar(blocks(block.children, width - 2, true));
        case Block::Kind::Alert: {
            Elements rows;
            rows.push_back(ftxui::bold(ftxui::text(alert_label(block.info))));
            if (!block.children.empty()) {
                rows.push_back(blocks(block.children, width - 2, true));
            }
            return with_bar(ftxui::vbox(std::move(rows)));
        }
        case Block::Kind::BulletList:
        case Block::Kind::OrderedList:
            return list(block, width);
        case Block::Kind::ListItem:
            return blocks(block.children, width, false);
        case Block::Kind::Code:
            return code(block, width);
        case Block::Kind::Rule:
            return ftxui::separator();
        case Block::Kind::Table:
            return table(block, width);
        case Block::Kind::FootnoteDef:
            return footnote(block, width);
        case Block::Kind::Document:
            return blocks(block.children, width, true);
        case Block::Kind::Paragraph:
            break;
        }
        return lines_element(flow(atoms_of(block, Look{}), width));
    }

    /// Barra "│ " tenue a la izquierda, del alto del contenido.
    static Element with_bar(Element content) {
        return ftxui::hbox({ftxui::dim(ftxui::separatorLight()), ftxui::text(" "),
                            std::move(content)});
    }

    /// Marcador a la izquierda y sangría colgante del mismo ancho.
    Element hanging(const std::string& marker, const std::vector<Block>& children, int width,
                    bool spaced) {
        const int marker_width = ftxui::string_width(marker);
        return ftxui::hbox({ftxui::text(marker), blocks(children, width - marker_width, spaced)});
    }

    Element list(const Block& block, int width) {
        static constexpr std::string_view kBullets[] = {"•", "◦", "▪"};
        const bool ordered = block.kind == Block::Kind::OrderedList;
        const std::string bullet{kBullets[static_cast<std::size_t>(bullet_depth_ % 3)]};
        const unsigned last = block.start + static_cast<unsigned>(block.children.size()) -
                              (block.children.empty() ? 0U : 1U);
        const int number_width = ftxui::string_width(std::to_string(last));
        if (!ordered) {
            ++bullet_depth_;
        }
        Elements rows;
        unsigned number = block.start;
        for (const Block& item : block.children) {
            std::string marker;
            if (ordered) {
                const std::string digits = std::to_string(number++);
                marker = std::string(static_cast<std::size_t>(std::max(
                                         0, number_width - ftxui::string_width(digits))),
                                     ' ') +
                         digits + ". ";
                if (item.task) {
                    marker += item.checked ? "☑ " : "☐ ";
                }
            } else if (item.task) {
                marker = item.checked ? "☑ " : "☐ ";
            } else {
                marker = bullet + " ";
            }
            rows.push_back(hanging(marker, item.children, width, false));
        }
        if (!ordered) {
            --bullet_depth_;
        }
        return ftxui::vbox(std::move(rows));
    }

    static Element code(const Block& block, int width) {
        // El marco y un espacio de margen a cada lado ocupan cuatro columnas.
        const int inner = std::max(width - 4, 1);
        std::vector<Line> lines;
        std::string_view rest = block.code;
        if (!rest.empty() && rest.back() == '\n') {
            rest.remove_suffix(1);
        }
        while (true) {
            const std::size_t end = rest.find('\n');
            const std::string_view source = rest.substr(0, end);
            lines.emplace_back();
            for (const std::string& glyph : ftxui::Utf8ToGlyphs(source)) {
                if (glyph.empty()) {
                    continue;
                }
                const int glyph_width = ftxui::string_width(glyph);
                if (lines.back().width + glyph_width > inner && lines.back().width > 0) {
                    lines.emplace_back(); // Línea larga: se parte, sin reacomodar.
                }
                lines.back().append(glyph, glyph_width, Look{});
            }
            if (end == std::string_view::npos) {
                break;
            }
            rest.remove_prefix(end + 1);
        }
        Element content = ftxui::hbox(
            {ftxui::text(" "), ftxui::flex(lines_element(lines)), ftxui::text(" ")});
        if (block.info.empty()) {
            return ftxui::borderRounded(std::move(content));
        }
        return ftxui::window(ftxui::dim(ftxui::text(" " + block.info + " ")), std::move(content));
    }

    Element footnote(const Block& block, int width) {
        return hanging("[" + std::to_string(block.footnote) + "] ", block.children, width, true);
    }

    Element table(const Block& block, int width) {
        std::size_t columns = block.align.size();
        for (const auto& row : block.rows) {
            columns = std::max(columns, row.size());
        }
        if (columns == 0) {
            return ftxui::text("");
        }
        // Átomos de cada celda (el encabezado en negritas).
        std::vector<std::vector<std::vector<Atom>>> cells;
        std::vector<int> natural(columns, 1);
        for (std::size_t r = 0; r < block.rows.size(); ++r) {
            Look base;
            base.bold = r < block.header_rows;
            auto& row_atoms = cells.emplace_back(columns);
            for (std::size_t c = 0; c < block.rows[r].size(); ++c) {
                row_atoms[c] = atoms_of(block.rows[r][c], base);
                for (const Line& line : flow(row_atoms[c], 1 << 20)) {
                    natural[c] = std::max(natural[c], line.width);
                }
            }
        }
        // "│ a │ b │": tres columnas por celda más la barra inicial.
        const int count = static_cast<int>(columns);
        const int available = width - 3 * count - 1;
        constexpr int kMinColumn = 6;
        if (available < kMinColumn * count) {
            return cards(block, cells, width);
        }
        const std::vector<int> widths = distribute(natural, available);

        std::vector<Line> lines;
        const auto rule = [&](std::string_view left, std::string_view mid,
                              std::string_view right) {
            std::string text{left};
            for (std::size_t c = 0; c < columns; ++c) {
                text += repeat("─", widths[c] + 2);
                text += c + 1 < columns ? mid : right;
            }
            Line line;
            Look look;
            look.dim = true;
            line.append(text, look);
            lines.push_back(std::move(line));
        };
        Look border;
        border.dim = true;
        std::vector<std::vector<std::vector<Line>>> wrapped(cells.size());
        bool multiline = false;
        for (std::size_t r = 0; r < cells.size(); ++r) {
            for (std::size_t c = 0; c < columns; ++c) {
                wrapped[r].push_back(flow(cells[r][c], widths[c]));
                multiline = multiline || wrapped[r].back().size() > 1;
            }
        }
        rule("┌", "┬", "┐");
        for (std::size_t r = 0; r < wrapped.size(); ++r) {
            if (r > 0 && (r == block.header_rows || multiline)) {
                rule("├", "┼", "┤");
            }
            std::size_t height = 1;
            for (const auto& cell : wrapped[r]) {
                height = std::max(height, cell.size());
            }
            for (std::size_t y = 0; y < height; ++y) {
                Line line;
                line.append("│", 1, border);
                for (std::size_t c = 0; c < columns; ++c) {
                    const auto& cell = wrapped[r][c];
                    const Line empty;
                    const Line& part = y < cell.size() ? cell[y] : empty;
                    const int gap = widths[c] - part.width;
                    const Align align = c < block.align.size() ? block.align[c] : Align::Default;
                    int left = 0;
                    if (align == Align::Right) {
                        left = gap;
                    } else if (align == Align::Center) {
                        left = gap / 2;
                    }
                    pad(line, 1 + left);
                    for (const Segment& segment : part.segments) {
                        line.append(segment.text, segment.look);
                    }
                    pad(line, gap - left + 1);
                    line.append("│", 1, border);
                }
                lines.push_back(std::move(line));
            }
        }
        rule("└", "┴", "┘");
        return lines_element(lines);
    }

    /// Reparte el ancho: cada columna recibe su ancho natural hasta un tope
    /// común; lo que sobra se da a las columnas recortadas.
    static std::vector<int> distribute(const std::vector<int>& natural, int available) {
        int total = 0;
        for (const int w : natural) {
            total += w;
        }
        if (total <= available) {
            return natural;
        }
        const auto used = [&](int cap) {
            int sum = 0;
            for (const int w : natural) {
                sum += std::min(w, cap);
            }
            return sum;
        };
        int low = 1;
        int high = *std::max_element(natural.begin(), natural.end());
        while (low < high) { // Mayor tope que todavía cabe.
            const int mid = (low + high + 1) / 2;
            if (used(mid) <= available) {
                low = mid;
            } else {
                high = mid - 1;
            }
        }
        std::vector<int> widths;
        int left = available - used(low);
        for (const int w : natural) {
            int width = std::min(w, low);
            if (w > low && left > 0) {
                ++width;
                --left;
            }
            widths.push_back(width);
        }
        return widths;
    }

    /// Tabla demasiado ancha: una tarjeta por fila con "Encabezado: valor".
    static Element cards(const Block& block,
                         const std::vector<std::vector<std::vector<Atom>>>& cells, int width) {
        Elements rows;
        const std::size_t first = block.header_rows > 0 ? block.header_rows : 0;
        for (std::size_t r = first; r < cells.size(); ++r) {
            if (!rows.empty()) {
                rows.push_back(ftxui::dim(ftxui::separatorLight()));
            }
            for (std::size_t c = 0; c < cells[r].size(); ++c) {
                std::vector<Atom> atoms;
                if (block.header_rows > 0) {
                    atoms = cells[0][c];
                } else {
                    Look bold;
                    bold.bold = true;
                    push_words(atoms, "Columna " + std::to_string(c + 1), bold);
                }
                if (!atoms.empty()) {
                    atoms.back().text += ":";
                } else {
                    Look bold;
                    bold.bold = true;
                    atoms.push_back(Atom{Atom::Kind::Word, ":", bold});
                }
                push_space(atoms, Look{});
                atoms.insert(atoms.end(), cells[r][c].begin(), cells[r][c].end());
                rows.push_back(lines_element(flow(atoms, width)));
            }
        }
        if (rows.empty()) { // Solo encabezado: también se muestra.
            for (std::size_t c = 0; c < cells.front().size(); ++c) {
                rows.push_back(lines_element(flow(cells.front()[c], width)));
            }
        }
        return ftxui::vbox(std::move(rows));
    }

    int bullet_depth_ = 0;
};

} // namespace

std::string hyperlink_target(std::string_view url) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(url.size());
    for (const char c : url) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte >= 0x21 && byte <= 0x7E) {
            out.push_back(c);
        } else {
            out.push_back('%');
            out.push_back(kHex[byte >> 4U]);
            out.push_back(kHex[byte & 0x0FU]);
        }
    }
    return out;
}

Element render_plain(std::string_view text, int width) {
    std::vector<Atom> atoms;
    push_words(atoms, sanitize(text), Look{}, false);
    return lines_element(flow(atoms, std::max(width, 1)));
}

Element render(const Document& document, int width) {
    width = std::max(width, 1);
    Renderer renderer;
    Element body = renderer.blocks(document.blocks, width, true);
    if (document.footnotes.empty()) {
        return body;
    }
    Elements rows;
    if (!document.blocks.empty()) {
        rows.push_back(std::move(body));
        rows.push_back(ftxui::text(""));
    }
    rows.push_back(ftxui::separator());
    rows.push_back(renderer.blocks(document.footnotes, width, false));
    return ftxui::vbox(std::move(rows));
}

} // namespace chatbot::cli::md
