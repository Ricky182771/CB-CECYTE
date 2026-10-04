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
    bool accent = false; ///< Subtítulos H3-H6.
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
    look.accent = look.accent || base.accent;
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

/// Medidas de un átomo para el ajuste. Los caracteres (para partir una
/// palabra más ancha que la línea) se calculan solo si hacen falta.
struct Measure {
    int width = 0;
    bool split = false; ///< glyphs y glyph_widths ya calculados.
    std::vector<std::string> glyphs;
    std::vector<int> glyph_widths;
};

std::vector<Measure> measure(const std::vector<Atom>& atoms) {
    std::vector<Measure> measures(atoms.size());
    for (std::size_t i = 0; i < atoms.size(); ++i) {
        if (atoms[i].kind != Atom::Kind::Break) {
            measures[i].width = ftxui::string_width(atoms[i].text);
        }
    }
    return measures;
}

void split_glyphs(const Atom& atom, Measure& measure) {
    if (measure.split) {
        return;
    }
    for (std::string& glyph : ftxui::Utf8ToGlyphs(atom.text)) {
        if (glyph.empty()) {
            continue; // Segunda celda de un carácter ancho.
        }
        measure.glyph_widths.push_back(ftxui::string_width(glyph));
        measure.glyphs.push_back(std::move(glyph));
    }
    measure.split = true;
}

/// Ajuste de líneas: una palabra que no cabe en la línea pasa a la
/// siguiente; si no cabe ni en una línea vacía, se parte por caracteres.
/// Toda línea tiene al menos un carácter, aunque sea ancho. El resultado va
/// a sink: Lines lo arma y LineCount solo cuenta, con el mismo cálculo.
template <typename Sink>
void layout(const std::vector<Atom>& atoms, std::vector<Measure>& measures, int width,
            Sink& sink) {
    width = std::max(width, 1);
    int line_width = 0;
    std::size_t pending = atoms.size(); // Espacio pendiente (índice), o ninguno.
    for (std::size_t i = 0; i < atoms.size(); ++i) {
        const Atom& atom = atoms[i];
        switch (atom.kind) {
        case Atom::Kind::Break:
            sink.new_line();
            line_width = 0;
            pending = atoms.size();
            continue;
        case Atom::Kind::Space:
            if (line_width > 0) {
                pending = i;
            }
            continue;
        case Atom::Kind::Word:
            break;
        }
        const bool has_space = pending < atoms.size();
        const int atom_width = measures[i].width;
        const int space = has_space ? measures[pending].width : 0;
        if (line_width + space + atom_width <= width) {
            if (has_space) {
                sink.add(atoms[pending].text, space, atoms[pending].look);
            }
            sink.add(atom.text, atom_width, atom.look);
            line_width += space + atom_width;
        } else if (atom_width <= width) {
            sink.new_line();
            sink.add(atom.text, atom_width, atom.look);
            line_width = atom_width;
        } else {
            // Más ancho que la línea: llena lo que queda y sigue partiendo.
            if (has_space && line_width + space < width) {
                sink.add(atoms[pending].text, space, atoms[pending].look);
                line_width += space;
            } else if (line_width > 0) {
                sink.new_line();
                line_width = 0;
            }
            Measure& parts = measures[i];
            split_glyphs(atom, parts);
            for (std::size_t k = 0; k < parts.glyphs.size(); ++k) {
                const int glyph_width = parts.glyph_widths[k];
                if (line_width + glyph_width > width && line_width > 0) {
                    sink.new_line();
                    line_width = 0;
                }
                sink.add(parts.glyphs[k], glyph_width, atom.look);
                line_width += glyph_width;
            }
        }
        pending = atoms.size();
    }
}

struct Lines {
    std::vector<Line> lines = std::vector<Line>(1);
    void new_line() { lines.emplace_back(); }
    void add(const std::string& text, int width, const Look& look) {
        lines.back().append(text, width, look);
    }
};

struct LineCount {
    int lines = 1;
    void new_line() { ++lines; }
    void add(const std::string& /*text*/, int /*width*/, const Look& /*look*/) {}
};

/// Ajusta los átomos a width columnas (ver layout()).
std::vector<Line> flow(const std::vector<Atom>& atoms, int width) {
    std::vector<Measure> measures = measure(atoms);
    Lines sink;
    layout(atoms, measures, width, sink);
    return std::move(sink.lines);
}

struct MaxWidth {
    int widest = 0;
    int current = 0;
    void new_line() { current = 0; }
    void add(const std::string& /*text*/, int width, const Look& /*look*/) {
        current += width;
        widest = std::max(widest, current);
    }
};

/// Ancho de la línea más larga sin ajustar (solo los saltos duros cortan).
int natural_width(const std::vector<Atom>& atoms) {
    std::vector<Measure> measures = measure(atoms);
    MaxWidth sink;
    layout(atoms, measures, 1 << 20, sink);
    return sink.widest;
}

/// Cuántas líneas da flow(atoms, width), sin armarlas. measures sale de
/// measure(atoms) y se reusa entre llamadas.
int count_lines(const std::vector<Atom>& atoms, std::vector<Measure>& measures, int width) {
    LineCount sink;
    layout(atoms, measures, width, sink);
    return sink.lines;
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
    if (look.code || look.accent) {
        // Cian de la paleta de 16 colores: cada tema de terminal lo ajusta
        // para que se lea sobre su fondo, claro u oscuro.
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

/// Ancho mínimo de una columna de tabla; si ni así cabe, la tabla se dibuja
/// como tarjetas.
constexpr int kMinColumn = 6;

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
            // H3-H6 en color en vez de tenues: dim casi no se lee sobre
            // fondos translúcidos.
            base.accent = block.level >= 3;
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

public:
    /// Lo que hace falta para dibujar una tabla: átomos de cada celda (el
    /// encabezado en negritas), ancho natural de cada columna y anchos
    /// repartidos (vacío si la tabla va como tarjetas).
    struct TableLayout {
        std::size_t columns = 0;
        std::vector<std::vector<std::vector<Atom>>> cells;
        std::vector<int> natural;
        std::vector<int> widths;
    };

    static TableLayout table_layout(const Block& block, int width) {
        TableLayout layout;
        for (const auto& row : block.rows) {
            layout.columns = std::max(layout.columns, row.size());
        }
        layout.columns = std::max(layout.columns, block.align.size());
        if (layout.columns == 0) {
            return layout;
        }
        layout.natural.assign(layout.columns, 1);
        for (std::size_t r = 0; r < block.rows.size(); ++r) {
            Look base;
            base.bold = r < block.header_rows;
            auto& row_atoms = layout.cells.emplace_back(layout.columns);
            for (std::size_t c = 0; c < block.rows[r].size(); ++c) {
                row_atoms[c] = atoms_of(block.rows[r][c], base);
                layout.natural[c] = std::max(layout.natural[c], natural_width(row_atoms[c]));
            }
        }
        // "│ a │ b │": tres columnas por celda más la barra inicial.
        const int count = static_cast<int>(layout.columns);
        const int available = width - 3 * count - 1;
        if (available >= kMinColumn * count) {
            layout.widths = distribute(layout.cells, layout.natural, available);
        }
        return layout;
    }

private:
    Element table(const Block& block, int width) {
        TableLayout layout = table_layout(block, width);
        if (layout.columns == 0) {
            return ftxui::text("");
        }
        if (layout.widths.empty()) {
            return cards(block, layout.cells, width);
        }
        const std::size_t columns = layout.columns;
        const auto& cells = layout.cells;
        const std::vector<int>& widths = layout.widths;

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

    /// Reparte el ancho de las columnas para que la tabla mida lo menos
    /// posible de alto (la suma, por fila, de su celda más alta, medida con
    /// el mismo flow() que dibuja). Si cabe con el ancho natural de cada
    /// columna, se usa ese. Si no:
    /// 1. Punto de partida: la menor altura H tal que todas las columnas
    ///    caben con todas sus celdas en H líneas o menos, y en cada columna
    ///    el menor ancho que lo logra. Ese ancho no baja de la palabra más
    ///    larga de la columna (para no partir palabras si se puede); si así
    ///    no caben, ese piso se recorta parejo, hasta min(natural, kMinColumn).
    ///    Empezar todas en el mínimo no sirve: cuando varias columnas son a
    ///    la vez la más alta de una fila, ensanchar una sola no baja la
    ///    fila, ninguna mejora y todo el ancho se va a una columna.
    /// 2. Las columnas de pantalla que sobran se dan una por una a la columna
    ///    donde más baja el alto total; en empate, a la que más lejos está
    ///    de su ancho natural.
    static std::vector<int> distribute(const std::vector<std::vector<std::vector<Atom>>>& cells,
                                       const std::vector<int>& natural, int available) {
        int total = 0;
        for (const int w : natural) {
            total += w;
        }
        if (total <= available) {
            return natural;
        }
        const std::size_t columns = natural.size();
        const std::size_t rows = cells.size();
        // Medidas de cada celda: se calculan una vez para todos los anchos.
        std::vector<std::vector<std::vector<Measure>>> measures(rows);
        for (std::size_t r = 0; r < rows; ++r) {
            for (std::size_t c = 0; c < columns; ++c) {
                measures[r].push_back(measure(cells[r][c]));
            }
        }
        // Líneas de cada celda de la columna c con el ancho dado.
        const auto lines_at = [&](std::size_t c, int width) {
            std::vector<int> lines(rows);
            for (std::size_t r = 0; r < rows; ++r) {
                lines[r] = count_lines(cells[r][c], measures[r][c], width);
            }
            return lines;
        };
        const auto tallest = [&](std::size_t c, int width) {
            const std::vector<int> lines = lines_at(c, width);
            return lines.empty() ? 1 : *std::max_element(lines.begin(), lines.end());
        };

        // 1. Menor H con la que todo cabe. Con más ancho nunca hay más líneas
        //    (ajuste voraz), así que se puede buscar por bisección.
        // Piso de cada columna: su palabra más larga, entre el mínimo y el
        // ancho natural.
        std::vector<int> minimum(columns);
        std::vector<int> word(columns);
        for (std::size_t c = 0; c < columns; ++c) {
            minimum[c] = std::min(natural[c], kMinColumn);
            int longest = 0;
            for (std::size_t r = 0; r < rows; ++r) {
                for (const Atom& atom : cells[r][c]) {
                    if (atom.kind == Atom::Kind::Word) {
                        longest = std::max(longest, ftxui::string_width(atom.text));
                    }
                }
            }
            word[c] = std::clamp(longest, minimum[c], natural[c]);
        }
        const auto floors_with = [&](int cap) {
            int sum = 0;
            for (std::size_t c = 0; c < columns; ++c) {
                sum += std::max(minimum[c], std::min(word[c], cap));
            }
            return sum;
        };
        int cap = kMinColumn; // Con este tope los pisos son el mínimo: caben.
        int cap_high = *std::max_element(word.begin(), word.end());
        while (cap < cap_high) { // Mayor tope con el que los pisos caben.
            const int mid = cap + (cap_high - cap + 1) / 2;
            if (floors_with(mid) <= available) {
                cap = mid;
            } else {
                cap_high = mid - 1;
            }
        }
        int highest = 1;
        for (std::size_t c = 0; c < columns; ++c) {
            minimum[c] = std::max(minimum[c], std::min(word[c], cap));
            highest = std::max(highest, tallest(c, minimum[c]));
        }
        // Anchos para que ninguna celda pase de h líneas; false si no caben.
        const auto widths_for = [&](int h, std::vector<int>& out) {
            int sum = 0;
            for (std::size_t c = 0; c < columns; ++c) {
                int low = minimum[c];
                int high = natural[c];
                if (tallest(c, high) > h) {
                    return false;
                }
                while (low < high) {
                    const int mid = low + (high - low) / 2;
                    if (tallest(c, mid) <= h) {
                        high = mid;
                    } else {
                        low = mid + 1;
                    }
                }
                out[c] = low;
                sum += low;
            }
            return sum <= available;
        };
        std::vector<int> widths = minimum; // Con h = highest siempre cabe.
        int low_h = 1;
        int high_h = highest;
        std::vector<int> candidate(columns);
        while (low_h < high_h) {
            const int mid = low_h + (high_h - low_h) / 2;
            if (widths_for(mid, candidate)) {
                high_h = mid;
            } else {
                low_h = mid + 1;
            }
        }
        if (widths_for(low_h, candidate)) {
            widths = candidate;
        }
        int left = available;
        for (const int w : widths) {
            left -= w;
        }

        // 2. Lo que sobra, columna por columna.
        // current: con el ancho actual; wider: con una columna más (vacío si
        // la columna ya tiene su ancho natural). Solo se recalcula la columna
        // que cambia.
        std::vector<std::vector<int>> current(columns);
        std::vector<std::vector<int>> wider(columns);
        for (std::size_t c = 0; c < columns; ++c) {
            current[c] = lines_at(c, widths[c]);
            if (widths[c] < natural[c]) {
                wider[c] = lines_at(c, widths[c] + 1);
            }
        }
        // Alto total si la columna changed tuviera las líneas dadas.
        const auto height_with = [&](std::size_t changed, const std::vector<int>& lines) {
            int height = 0;
            for (std::size_t r = 0; r < rows; ++r) {
                int row = lines[r];
                for (std::size_t c = 0; c < columns; ++c) {
                    if (c != changed) {
                        row = std::max(row, current[c][r]);
                    }
                }
                height += row;
            }
            return height;
        };
        while (left > 0) {
            const int height = columns > 0 ? height_with(0, current[0]) : 0;
            std::size_t best = columns;
            int best_gain = 0;
            int best_missing = 0;
            for (std::size_t c = 0; c < columns; ++c) {
                if (widths[c] >= natural[c]) {
                    continue;
                }
                const int gain = height - height_with(c, wider[c]);
                const int missing = natural[c] - widths[c];
                if (best == columns || gain > best_gain ||
                    (gain == best_gain && missing > best_missing)) {
                    best = c;
                    best_gain = gain;
                    best_missing = missing;
                }
            }
            if (best == columns) {
                break; // Todas tienen su ancho natural (no pasa si no cabía).
            }
            ++widths[best];
            --left;
            current[best] = std::move(wider[best]);
            wider[best].clear();
            if (widths[best] < natural[best]) {
                wider[best] = lines_at(best, widths[best] + 1);
            }
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

std::vector<int> table_column_widths(const Block& table, int width) {
    return Renderer::table_layout(table, std::max(width, 1)).widths;
}

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
