#include "markdown.h"

#include <md4c.h>

#include <array>
#include <cstddef>
#include <limits>
#include <utility>

namespace chatbot::cli::md {
namespace {

/// Dialecto: GitHub (tablas, tachado, tareas, autolinks, alertas, notas al
/// pie) + LaTeX + ==resaltado==, sin HTML crudo (se muestra como texto). Sin
/// MD_FLAG_UNDERLINE, que desactivaría '_' como énfasis.
constexpr unsigned kParserFlags =
    MD_DIALECT_GITHUB | MD_FLAG_LATEXMATHSPANS | MD_FLAG_HIGHLIGHT | MD_FLAG_NOHTML;

constexpr std::string_view kReplacement = "\xEF\xBF\xBD"; // U+FFFD

void append_utf8(std::string& out, char32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

/// Entidades con nombre que se decodifican; el resto se deja tal cual.
constexpr std::array<std::pair<std::string_view, char32_t>, 17> kNamedEntities{{
    {"amp", U'&'},       {"lt", U'<'},        {"gt", U'>'},        {"quot", U'"'},
    {"apos", U'\''},     {"nbsp", 0x00A0},    {"copy", 0x00A9},    {"reg", 0x00AE},
    {"hellip", 0x2026},  {"mdash", 0x2014},   {"ndash", 0x2013},   {"laquo", 0x00AB},
    {"raquo", 0x00BB},   {"deg", 0x00B0},     {"times", 0x00D7},   {"divide", 0x00F7},
    {"euro", 0x20AC},
}};

/// Texto de un atributo de md4c (URL, lenguaje, tipo de alerta), con sus
/// entidades decodificadas, filtrado y en una sola línea.
std::string attribute_text(const MD_ATTRIBUTE& attribute) {
    std::string raw;
    if (attribute.text == nullptr || attribute.size == 0) {
        return raw;
    }
    for (std::size_t i = 0; attribute.substr_offsets[i] < attribute.size; ++i) {
        const MD_OFFSET begin = attribute.substr_offsets[i];
        const MD_OFFSET end = attribute.substr_offsets[i + 1];
        const std::string_view piece{attribute.text + begin, end - begin};
        switch (attribute.substr_types[i]) {
        case MD_TEXT_ENTITY:
            raw += decode_entity(piece);
            break;
        case MD_TEXT_NULLCHAR:
            raw += kReplacement;
            break;
        default:
            raw += piece;
            break;
        }
    }
    std::string clean = sanitize(raw);
    for (char& c : clean) {
        if (c == '\n') {
            c = ' ';
        }
    }
    return clean;
}

/// Un contenedor abierto. implicit = párrafo creado para el texto suelto de
/// una lista compacta (o de una cita/alerta/nota sin párrafo).
struct Frame {
    Block* block;
    bool implicit;
};

/// Arma el árbol a partir de los callbacks de md4c. Los bloques se agregan
/// siempre al contenedor del tope, así que los punteros de la pila (todos
/// ancestros del tope) no se invalidan.
class Builder {
public:
    Builder() {
        root_.kind = Block::Kind::Document;
        footnotes_.kind = Block::Kind::Document;
        stack_.push_back(Frame{&root_, false});
    }

    void enter_block(MD_BLOCKTYPE type, void* detail) {
        close_implicit();
        switch (type) {
        case MD_BLOCK_DOC:
            return;
        case MD_BLOCK_FOOTNOTE_DEF_SECTION:
            stack_.push_back(Frame{&footnotes_, false});
            return;
        case MD_BLOCK_THEAD:
            in_table_head_ = true;
            return;
        case MD_BLOCK_TBODY:
            in_table_head_ = false;
            return;
        case MD_BLOCK_TR:
            if (Block* table = current_table()) {
                table->rows.emplace_back();
                if (in_table_head_) {
                    ++table->header_rows;
                }
            }
            return;
        case MD_BLOCK_TH:
        case MD_BLOCK_TD:
            enter_cell(static_cast<const MD_BLOCK_TD_DETAIL*>(detail));
            return;
        default:
            break;
        }
        Block block;
        switch (type) {
        case MD_BLOCK_QUOTE:
            block.kind = Block::Kind::Quote;
            break;
        case MD_BLOCK_ADMONITION:
            block.kind = Block::Kind::Alert;
            block.info = lower(attribute_text(static_cast<MD_BLOCK_ADMONITION_DETAIL*>(detail)->type));
            break;
        case MD_BLOCK_UL:
            block.kind = Block::Kind::BulletList;
            break;
        case MD_BLOCK_OL:
            block.kind = Block::Kind::OrderedList;
            block.start = static_cast<MD_BLOCK_OL_DETAIL*>(detail)->start;
            break;
        case MD_BLOCK_LI: {
            const auto* li = static_cast<MD_BLOCK_LI_DETAIL*>(detail);
            block.kind = Block::Kind::ListItem;
            block.task = li->is_task != 0;
            block.checked = block.task && (li->task_mark == 'x' || li->task_mark == 'X');
            break;
        }
        case MD_BLOCK_HR:
            block.kind = Block::Kind::Rule;
            break;
        case MD_BLOCK_H:
            block.kind = Block::Kind::Heading;
            block.level = static_cast<MD_BLOCK_H_DETAIL*>(detail)->level;
            break;
        case MD_BLOCK_CODE:
            block.kind = Block::Kind::Code;
            block.info = attribute_text(static_cast<MD_BLOCK_CODE_DETAIL*>(detail)->lang);
            break;
        case MD_BLOCK_TABLE:
            block.kind = Block::Kind::Table;
            break;
        case MD_BLOCK_FOOTNOTE_DEF:
            block.kind = Block::Kind::FootnoteDef;
            block.footnote = static_cast<MD_BLOCK_FOOTNOTE_DEF_DETAIL*>(detail)->id;
            break;
        default: // MD_BLOCK_P, MD_BLOCK_HTML (no aparece con NOHTML) y otros.
            block.kind = Block::Kind::Paragraph;
            break;
        }
        Block& parent = *stack_.back().block;
        parent.children.push_back(std::move(block));
        stack_.push_back(Frame{&parent.children.back(), false});
    }

    void leave_block(MD_BLOCKTYPE type) {
        close_implicit();
        switch (type) {
        case MD_BLOCK_DOC:
        case MD_BLOCK_THEAD:
        case MD_BLOCK_TBODY:
        case MD_BLOCK_TR:
            return;
        default:
            break;
        }
        if (stack_.size() > 1) {
            stack_.pop_back();
        }
    }

    void enter_span(MD_SPANTYPE type, void* detail) {
        switch (type) {
        case MD_SPAN_EM:
            ++emphasis_;
            break;
        case MD_SPAN_STRONG:
            ++strong_;
            break;
        case MD_SPAN_DEL:
            ++strike_;
            break;
        case MD_SPAN_MARK:
            ++highlight_;
            break;
        case MD_SPAN_CODE:
            ++code_;
            break;
        case MD_SPAN_LATEXMATH:
            math_ = Run::Kind::Math;
            break;
        case MD_SPAN_LATEXMATH_DISPLAY:
            math_ = Run::Kind::MathDisplay;
            break;
        case MD_SPAN_A: {
            const auto* a = static_cast<MD_SPAN_A_DETAIL*>(detail);
            Block& block = text_block();
            block.links.push_back(Link{attribute_text(a->href), a->is_autolink != 0});
            links_.push_back(static_cast<int>(block.links.size()) - 1);
            break;
        }
        case MD_SPAN_IMG: {
            Run run;
            run.kind = Run::Kind::Image;
            run.url = attribute_text(static_cast<MD_SPAN_IMG_DETAIL*>(detail)->src);
            text_block().runs.push_back(std::move(run));
            ++image_;
            break;
        }
        case MD_SPAN_FOOTNOTE_REF: {
            Run run;
            run.kind = Run::Kind::FootnoteRef;
            run.footnote = static_cast<MD_SPAN_FOOTNOTE_REF_DETAIL*>(detail)->id;
            text_block().runs.push_back(std::move(run));
            ++footnote_ref_;
            break;
        }
        default:
            break;
        }
    }

    void leave_span(MD_SPANTYPE type) {
        const auto down = [](int& counter) { counter = counter > 0 ? counter - 1 : 0; };
        switch (type) {
        case MD_SPAN_EM:
            down(emphasis_);
            break;
        case MD_SPAN_STRONG:
            down(strong_);
            break;
        case MD_SPAN_DEL:
            down(strike_);
            break;
        case MD_SPAN_MARK:
            down(highlight_);
            break;
        case MD_SPAN_CODE:
            down(code_);
            break;
        case MD_SPAN_LATEXMATH:
        case MD_SPAN_LATEXMATH_DISPLAY:
            math_ = Run::Kind::Text;
            break;
        case MD_SPAN_A:
            if (!links_.empty()) {
                links_.pop_back();
            }
            break;
        case MD_SPAN_IMG:
            down(image_);
            break;
        case MD_SPAN_FOOTNOTE_REF:
            down(footnote_ref_);
            break;
        default:
            break;
        }
    }

    void text(MD_TEXTTYPE type, std::string_view raw) {
        Block& top = *stack_.back().block;
        if (top.kind == Block::Kind::Code && !stack_.back().implicit) {
            top.code += type == MD_TEXT_NULLCHAR ? std::string{kReplacement} : sanitize(raw);
            return;
        }
        if (footnote_ref_ > 0) {
            return; // La etiqueta de la referencia se dibuja como [n].
        }
        if (type == MD_TEXT_BR) {
            Run run;
            run.kind = Run::Kind::LineBreak;
            text_block().runs.push_back(std::move(run));
            return;
        }
        std::string content;
        switch (type) {
        case MD_TEXT_SOFTBR:
            content = " ";
            break;
        case MD_TEXT_NULLCHAR:
            content = std::string{kReplacement};
            break;
        case MD_TEXT_ENTITY:
            content = decode_entity(raw);
            break;
        default: // NORMAL, CODE, LATEXMATH, HTML (literal).
            content = sanitize(raw);
            break;
        }
        Block& block = text_block();
        if (image_ > 0 && !block.runs.empty() && block.runs.back().kind == Run::Kind::Image) {
            block.runs.back().text += content; // Texto alternativo.
            return;
        }
        Run run;
        run.kind = code_ > 0 ? Run::Kind::Code : math_;
        run.text = std::move(content);
        run.style = static_cast<std::uint8_t>((emphasis_ > 0 ? kEmphasis : 0) |
                                              (strong_ > 0 ? kStrong : 0) |
                                              (strike_ > 0 ? kStrike : 0) |
                                              (highlight_ > 0 ? kHighlight : 0));
        run.link = links_.empty() ? -1 : links_.back();
        // Junta tramos contiguos iguales para no fragmentar las palabras.
        if (!block.runs.empty()) {
            Run& last = block.runs.back();
            if (last.kind == run.kind && last.style == run.style && last.link == run.link &&
                (run.kind == Run::Kind::Text || run.kind == Run::Kind::Code ||
                 run.kind == Run::Kind::Math || run.kind == Run::Kind::MathDisplay)) {
                last.text += run.text;
                return;
            }
        }
        block.runs.push_back(std::move(run));
    }

    Document finish() {
        Document document;
        document.blocks = std::move(root_.children);
        document.footnotes = std::move(footnotes_.children);
        return document;
    }

private:
    static std::string lower(std::string text) {
        for (char& c : text) {
            if (c >= 'A' && c <= 'Z') {
                c = static_cast<char>(c - 'A' + 'a');
            }
        }
        return text;
    }

    /// Bloque que recibe el texto: el del tope si es de texto; si no (lista
    /// compacta, cita, alerta, nota), un párrafo implícito.
    Block& text_block() {
        Block& top = *stack_.back().block;
        if (top.kind == Block::Kind::Paragraph || top.kind == Block::Kind::Heading) {
            return top;
        }
        Block paragraph;
        paragraph.kind = Block::Kind::Paragraph;
        top.children.push_back(std::move(paragraph));
        stack_.push_back(Frame{&top.children.back(), true});
        return top.children.back();
    }

    void close_implicit() {
        if (stack_.size() > 1 && stack_.back().implicit) {
            stack_.pop_back();
        }
    }

    Block* current_table() {
        for (auto it = stack_.rbegin(); it != stack_.rend(); ++it) {
            if (it->block->kind == Block::Kind::Table) {
                return it->block;
            }
        }
        return nullptr;
    }

    void enter_cell(const MD_BLOCK_TD_DETAIL* detail) {
        Block* table = current_table();
        if (table == nullptr || table->rows.empty()) {
            // Celda fuera de una tabla (no debería pasar): párrafo normal.
            Block& parent = *stack_.back().block;
            parent.children.emplace_back();
            stack_.push_back(Frame{&parent.children.back(), false});
            return;
        }
        if (table->rows.size() == 1) {
            Align align = Align::Default;
            switch (detail->align) {
            case MD_ALIGN_LEFT:
                align = Align::Left;
                break;
            case MD_ALIGN_CENTER:
                align = Align::Center;
                break;
            case MD_ALIGN_RIGHT:
                align = Align::Right;
                break;
            default:
                break;
            }
            table->align.push_back(align);
        }
        table->rows.back().emplace_back();
        stack_.push_back(Frame{&table->rows.back().back(), false});
    }

    Block root_;
    Block footnotes_;
    std::vector<Frame> stack_;
    bool in_table_head_ = false;
    int emphasis_ = 0;
    int strong_ = 0;
    int strike_ = 0;
    int highlight_ = 0;
    int code_ = 0;
    int image_ = 0;
    int footnote_ref_ = 0;
    Run::Kind math_ = Run::Kind::Text;
    std::vector<int> links_;
};

// Callbacks de C: ninguna excepción puede cruzarlos. Si algo falla, se aborta
// el parseo (devolver distinto de 0) y parse() usa el texto plano.
int on_enter_block(MD_BLOCKTYPE type, void* detail, void* userdata) {
    try {
        static_cast<Builder*>(userdata)->enter_block(type, detail);
        return 0;
    } catch (...) {
        return 1;
    }
}

int on_leave_block(MD_BLOCKTYPE type, void* /*detail*/, void* userdata) {
    try {
        static_cast<Builder*>(userdata)->leave_block(type);
        return 0;
    } catch (...) {
        return 1;
    }
}

int on_enter_span(MD_SPANTYPE type, void* detail, void* userdata) {
    try {
        static_cast<Builder*>(userdata)->enter_span(type, detail);
        return 0;
    } catch (...) {
        return 1;
    }
}

int on_leave_span(MD_SPANTYPE type, void* /*detail*/, void* userdata) {
    try {
        static_cast<Builder*>(userdata)->leave_span(type);
        return 0;
    } catch (...) {
        return 1;
    }
}

int on_text(MD_TEXTTYPE type, const MD_CHAR* text, MD_SIZE size, void* userdata) {
    try {
        static_cast<Builder*>(userdata)->text(type, std::string_view{text, size});
        return 0;
    } catch (...) {
        return 1;
    }
}

/// Documento de respaldo: todo el texto como un párrafo.
Document plain_document(std::string_view markdown) {
    Document document;
    std::string text = sanitize(markdown);
    if (text.find_first_not_of(" \n") != std::string::npos) {
        Block paragraph;
        paragraph.kind = Block::Kind::Paragraph;
        Run run;
        run.text = std::move(text);
        paragraph.runs.push_back(std::move(run));
        document.blocks.push_back(std::move(paragraph));
    }
    return document;
}

} // namespace

std::string decode_entity(std::string_view entity) {
    if (entity.size() < 3 || entity.front() != '&' || entity.back() != ';') {
        return std::string{entity};
    }
    const std::string_view name = entity.substr(1, entity.size() - 2);
    if (name.front() == '#') {
        const bool hex = name.size() > 1 && (name[1] == 'x' || name[1] == 'X');
        const std::string_view digits = name.substr(hex ? 2 : 1);
        if (digits.empty() || digits.size() > 8) {
            return std::string{entity};
        }
        char32_t cp = 0;
        for (const char c : digits) {
            unsigned value = 0;
            if (c >= '0' && c <= '9') {
                value = static_cast<unsigned>(c - '0');
            } else if (hex && c >= 'a' && c <= 'f') {
                value = static_cast<unsigned>(c - 'a' + 10);
            } else if (hex && c >= 'A' && c <= 'F') {
                value = static_cast<unsigned>(c - 'A' + 10);
            } else {
                return std::string{entity};
            }
            cp = cp * (hex ? 16U : 10U) + value;
        }
        if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            return std::string{kReplacement};
        }
        std::string out;
        append_utf8(out, cp);
        return sanitize(out); // Un &#27; (ESC) también se filtra.
    }
    for (const auto& [known, cp] : kNamedEntities) {
        if (name == known) {
            std::string out;
            append_utf8(out, cp);
            return out;
        }
    }
    return std::string{entity};
}

std::string sanitize(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size()) {
        const auto byte = static_cast<unsigned char>(text[i]);
        if (byte < 0x80) {
            if (byte == '\n') {
                out.push_back('\n');
            } else if (byte == '\t') {
                out += "    ";
            } else if (byte == '\r') {
                // Se quita: con '\r' se podría sobrescribir lo ya dibujado.
            } else if (byte < 0x20 || byte == 0x7F) {
                out += kReplacement; // C0, ESC y DEL.
            } else {
                out.push_back(static_cast<char>(byte));
            }
            ++i;
            continue;
        }
        // Secuencia UTF-8: se valida completa (sin formas largas, sin
        // sustitutos, hasta U+10FFFF).
        std::size_t length = 0;
        char32_t cp = 0;
        char32_t minimum = 0;
        if ((byte & 0xE0) == 0xC0) {
            length = 2;
            cp = byte & 0x1F;
            minimum = 0x80;
        } else if ((byte & 0xF0) == 0xE0) {
            length = 3;
            cp = byte & 0x0F;
            minimum = 0x800;
        } else if ((byte & 0xF8) == 0xF0) {
            length = 4;
            cp = byte & 0x07;
            minimum = 0x10000;
        }
        bool valid = length > 0 && i + length <= text.size();
        for (std::size_t k = 1; valid && k < length; ++k) {
            const auto next = static_cast<unsigned char>(text[i + k]);
            if ((next & 0xC0) != 0x80) {
                valid = false;
            } else {
                cp = (cp << 6) | (next & 0x3F);
            }
        }
        valid = valid && cp >= minimum && cp <= 0x10FFFF && !(cp >= 0xD800 && cp <= 0xDFFF);
        if (!valid) {
            out += kReplacement;
            ++i;
            continue;
        }
        if (cp >= 0x80 && cp <= 0x9F) {
            out += kReplacement; // C1 (incluye el CSI de 8 bits).
        } else {
            out.append(text.substr(i, length));
        }
        i += length;
    }
    return out;
}

Document parse(std::string_view markdown) {
    try {
        if (markdown.size() > std::numeric_limits<MD_SIZE>::max()) {
            return plain_document(markdown);
        }
        Builder builder;
        MD_PARSER parser{};
        parser.abi_version = 0;
        parser.flags = kParserFlags;
        parser.enter_block = on_enter_block;
        parser.leave_block = on_leave_block;
        parser.enter_span = on_enter_span;
        parser.leave_span = on_leave_span;
        parser.text = on_text;
        parser.debug_log = nullptr;
        parser.syntax = nullptr;
        const int result = md_parse(markdown.data(), static_cast<MD_SIZE>(markdown.size()), &parser,
                                    &builder);
        if (result != 0) {
            return plain_document(markdown);
        }
        return builder.finish();
    } catch (...) {
        try {
            return plain_document(markdown);
        } catch (...) {
            return Document{};
        }
    }
}

} // namespace chatbot::cli::md
