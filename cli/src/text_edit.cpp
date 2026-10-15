#include "text_edit.h"

#include "chatbot/utf8.h"

#include <ftxui/screen/string.hpp>

#include <algorithm>
#include <utility>

namespace chatbot::cli {

namespace {

/// Entradas de la caché de filas: el ancho del campo y el de una columna
/// menos (con barra) bastan; una más por un redimensionado.
constexpr std::size_t kCacheEntries = 3;

/// Un código del texto o un byte inválido: los bytes [start, end).
struct Token {
    std::size_t end = 0;
    bool valid = false;
    std::uint32_t code = 0;
};

Token token_at(std::string_view text, std::size_t offset) {
    std::size_t next = offset;
    if (const std::optional<std::uint32_t> code = utf8::next_code_point(text, next)) {
        return {next, true, *code};
    }
    return {offset + 1, false, 0};
}

bool is_continuation(char byte) { return (static_cast<unsigned char>(byte) & 0xC0U) == 0x80U; }

/// Inicio del código (o byte inválido) que termina en offset (> 0). Igual
/// que token_at hacia adelante: un byte que no es de continuación siempre
/// empieza un código, y un código válido mide a lo más 4 bytes.
std::size_t token_before(std::string_view text, std::size_t offset) {
    std::size_t start = offset - 1;
    for (int steps = 0; start > 0 && steps < 3 && is_continuation(text[start]); ++steps) {
        --start;
    }
    std::size_t next = start;
    if (utf8::next_code_point(text, next).has_value() && next == offset) {
        return start;
    }
    return offset - 1;
}

/// Controles que FTXUI no dibuja (C0 salvo "\n" y "\t", DEL y C1).
bool is_control(std::uint32_t code) {
    return (code < 0x20 && code != '\n' && code != '\t') || (code >= 0x7F && code < 0xA0);
}

/// Celdas de un código que no es control, con la tabla de FTXUI: 0 si es
/// combinante. Debajo de U+0300 no hay combinantes ni anchos (lo revisa una
/// prueba), y así el texto común no pasa por la búsqueda en la tabla.
int code_width(std::string_view text, std::size_t start, const Token& token) {
    if (token.code < 0x300) {
        return 1;
    }
    return ftxui::string_width(text.substr(start, token.end - start));
}

bool is_combining(std::string_view text, std::size_t start, const Token& token) {
    return token.valid && !is_control(token.code) && token.code != '\n' &&
           token.code != '\t' && code_width(text, start, token) == 0;
}

/// Un espacio " " (con los combinantes que traiga): una celda.
bool is_plain_space(std::string_view text, const TextGlyph& glyph) {
    return glyph.kind == GlyphKind::Normal && glyph.base_end == glyph.start + 1 &&
           text[glyph.start] == ' ';
}

/// Donde se puede cortar una fila: después de un espacio o de un "\t".
bool is_space(std::string_view text, const TextGlyph& glyph) {
    return glyph.kind == GlyphKind::Tab || is_plain_space(text, glyph);
}

/// El menor límite de glifo que no es menor que offset.
std::size_t glyph_boundary_at_or_after(std::string_view text, std::size_t offset) {
    const std::size_t before = glyph_boundary_at_or_before(text, offset);
    return before == std::min(offset, text.size()) ? before : next_glyph(text, before);
}

/// Una fila que empieza en row_start (principio de una línea o después de un
/// corte suave) de la línea lógica que termina en line_end (su "\n" o el
/// final del texto); last: cómo termina la línea.
TextRow layout_row(std::string_view text, std::size_t row_start, std::size_t line_end,
                   RowBreak last, int width) {
    const int usable = width - 1; // La última columna es del cursor.
    int used = 0;
    std::size_t at = row_start;
    // Después del último espacio de la fila, y lo que medía hasta ahí.
    std::size_t after_space = row_start;
    int width_to_space = 0;
    while (at < line_end) {
        const TextGlyph glyph = glyph_at(text, at);
        // El primer glifo de la fila siempre entra (con usable < 1, o si es
        // más ancho que la fila): así nunca hay una fila vacía en medio ni un
        // ciclo infinito.
        if (at > row_start && used + glyph.width > usable) {
            // Un espacio que ya no cabe se queda al final de la fila, en la
            // columna reservada (no se ve): ni baja la palabra que llenó la
            // fila ni empieza la siguiente. Solo si la fila llenó justo el
            // ancho útil (tras un glifo más ancho que la fila no hay columna
            // libre); si siguen más espacios, esos bajan. Si era lo último
            // de la línea, layout_from agrega después una fila vacía.
            if (is_plain_space(text, glyph) && used == usable) {
                return {row_start, glyph.end, used + glyph.width, RowBreak::Soft, true};
            }
            // Un "\t" que no cabe (4 celdas) baja; otro glifo se lleva la
            // palabra desde el último espacio.
            if (!is_space(text, glyph) && after_space > row_start) {
                return {row_start, after_space, width_to_space, RowBreak::Soft};
            }
            return {row_start, at, used, RowBreak::Soft};
        }
        used += glyph.width;
        at = glyph.end;
        if (is_space(text, glyph)) {
            after_space = at;
            width_to_space = used;
        }
    }
    return {row_start, line_end, used, last};
}

/// Agrega a out las filas desde row_start (el principio de una fila) hasta
/// el final del texto, o hasta que stop(inicio de la fila siguiente) dé true.
template <typename Stop>
void layout_from(std::string_view text, std::size_t row_start, int width,
                 std::vector<TextRow>& out, Stop stop) {
    std::size_t line_end = text.find('\n', row_start);
    while (true) {
        const bool newline = line_end != std::string_view::npos;
        const TextRow row = layout_row(text, row_start, newline ? line_end : text.size(),
                                       newline ? RowBreak::Newline : RowBreak::End, width);
        out.push_back(row);
        if (row.ends == RowBreak::End) {
            return;
        }
        std::size_t next = row.end;
        if (row.ends == RowBreak::Newline) {
            next = row.end + 1;
            line_end = text.find('\n', next);
        }
        if (stop(next)) {
            return;
        }
        row_start = next;
    }
}

/// Índice de la última fila que empieza en offset o antes. Los inicios de
/// las filas son estrictamente crecientes (una fila de corte suave nunca está
/// vacía).
int row_containing(const std::vector<TextRow>& rows, std::size_t offset) {
    const auto after = std::upper_bound(
        rows.begin(), rows.end(), offset,
        [](std::size_t value, const TextRow& row) { return value < row.start; });
    return std::max(0, static_cast<int>(after - rows.begin()) - 1);
}

/// Letras y dígitos: palabra. Espacios, puntuación y símbolos: no.
bool is_word_code(std::uint32_t code) {
    if (code < 0x80) {
        return (code >= '0' && code <= '9') || (code >= 'A' && code <= 'Z') ||
               (code >= 'a' && code <= 'z');
    }
    struct Range {
        std::uint32_t first;
        std::uint32_t last;
    };
    // Puntuación, signos y símbolos comunes; lo demás (letras de cualquier
    // alfabeto, ideogramas) cuenta como palabra, como en ftxui::Input.
    static constexpr Range kNotWord[] = {
        {0x80, 0xA9},     {0xAB, 0xB4},     {0xB6, 0xB9},   {0xBB, 0xBF},
        {0xD7, 0xD7},     {0xF7, 0xF7},     {0x2000, 0x206F}, {0x20A0, 0x20CF},
        {0x2100, 0x214F}, {0x2190, 0x2BFF}, {0x3000, 0x303F}, {0xFE30, 0xFE4F},
        {0xFF01, 0xFF0F}, {0xFF1A, 0xFF20}, {0xFF3B, 0xFF40}, {0xFF5B, 0xFF65},
        {0xFFF0, 0xFFFF}, {0x1F000, 0x1FAFF},
    };
    return std::none_of(std::begin(kNotWord), std::end(kNotWord), [code](const Range& range) {
        return code >= range.first && code <= range.last;
    });
}

} // namespace

TextGlyph glyph_at(std::string_view text, std::size_t offset) {
    const Token base = token_at(text, offset);
    TextGlyph glyph{offset, base.end, 1, GlyphKind::Normal, base.end};
    if (!base.valid) {
        glyph.kind = GlyphKind::Replacement;
    } else if (base.code == '\n') {
        glyph.kind = GlyphKind::Newline;
        glyph.width = 0;
        return glyph;
    } else if (base.code == '\t') {
        glyph.kind = GlyphKind::Tab;
        glyph.width = kTabWidth;
    } else if (is_control(base.code)) {
        glyph.kind = GlyphKind::Replacement;
    } else {
        glyph.width = code_width(text, offset, base);
        if (glyph.width == 0) {
            glyph.kind = GlyphKind::Baseless;
            glyph.width = 1;
        }
    }
    // Los combinantes que siguen son del mismo glifo.
    while (glyph.end < text.size()) {
        const Token next = token_at(text, glyph.end);
        if (!is_combining(text, glyph.end, next)) {
            break;
        }
        glyph.end = next.end;
    }
    return glyph;
}

std::size_t next_glyph(std::string_view text, std::size_t offset) {
    if (offset >= text.size()) {
        return text.size();
    }
    return glyph_at(text, offset).end;
}

std::size_t previous_glyph(std::string_view text, std::size_t offset) {
    offset = std::min(offset, text.size());
    if (offset == 0) {
        return 0;
    }
    std::size_t start = token_before(text, offset);
    if (!is_combining(text, start, token_at(text, start))) {
        return start;
    }
    // Una cadena de combinantes: el glifo empieza en su base, o en el primero
    // si no tiene base (principio del texto o después de un "\n").
    while (start > 0) {
        const std::size_t before = token_before(text, start);
        const Token token = token_at(text, before);
        if (is_combining(text, before, token)) {
            start = before;
            continue;
        }
        return token.valid && token.code == '\n' ? start : before;
    }
    return start;
}

std::size_t glyph_boundary_at_or_before(std::string_view text, std::size_t offset) {
    offset = std::min(offset, text.size());
    if (offset == text.size()) {
        return offset;
    }
    // Un código que no es combinante siempre empieza un glifo (un byte que
    // no es de continuación siempre empieza un código): desde el anterior se
    // avanza por glifos. Así el retroceso no pasa de la cadena de combinantes.
    std::size_t at = offset;
    while (at > 0 &&
           (is_continuation(text[at]) || is_combining(text, at, token_at(text, at)))) {
        --at;
    }
    while (at < offset) {
        const std::size_t next = next_glyph(text, at);
        if (next > offset) {
            break;
        }
        at = next;
    }
    return at;
}

std::string glyph_display(std::string_view text, const TextGlyph& glyph) {
    const std::string_view marks = text.substr(glyph.base_end, glyph.end - glyph.base_end);
    switch (glyph.kind) {
    case GlyphKind::Normal:
        return std::string{text.substr(glyph.start, glyph.end - glyph.start)};
    case GlyphKind::Newline:
        return {};
    case GlyphKind::Tab:
        return " ";
    case GlyphKind::Replacement:
        return std::string{utf8::kReplacement} + std::string{marks};
    case GlyphKind::Baseless:
        return " " + std::string{text.substr(glyph.start, glyph.end - glyph.start)};
    }
    return {};
}

std::string glyph_cell(std::string_view text, const TextGlyph& glyph, int cell) {
    if (cell == 0) {
        return glyph_display(text, glyph);
    }
    return glyph.kind == GlyphKind::Tab ? " " : "";
}

std::vector<TextRow> layout(std::string_view text, int width) {
    std::vector<TextRow> rows;
    layout_from(text, 0, width, rows, [](std::size_t) { return false; });
    return rows;
}

std::vector<TextRow> relayout(const std::vector<TextRow>& old, std::string_view text, int width,
                              std::size_t start, std::size_t removed, std::size_t inserted) {
    if (old.empty()) {
        return layout(text, width);
    }
    // Cada fila depende solo de su inicio y del texto que sigue, hasta el
    // primer glifo que ya no cabe: a lo más el ancho útil más el glifo más
    // ancho (un "\t"). Una fila de la misma línea que empieza más cerca que
    // eso de la edición puede cambiar (borrar al principio de una fila sube
    // una palabra; un combinante insertado se junta con el glifo de antes):
    // se distribuye de nuevo desde ahí. Las demás no cambian.
    const int reach = std::max(0, width - 1) + kTabWidth;
    int first = row_containing(old, start);
    int cells = 0; // Celdas entre el inicio de la fila first y la edición.
    for (std::size_t at = old[static_cast<std::size_t>(first)].start; at < start;) {
        const TextGlyph glyph = glyph_at(text, at); // Antes de start, el texto no cambió.
        if (glyph.end > start) {
            break;
        }
        cells += glyph.width;
        at = glyph.end;
    }
    while (first > 0) {
        const TextRow& previous = old[static_cast<std::size_t>(first) - 1];
        if (previous.ends != RowBreak::Soft || cells + previous.width > reach) {
            break;
        }
        cells += previous.width;
        --first;
    }
    std::vector<TextRow> rows(old.begin(), old.begin() + first);
    // Después de lo editado, en cuanto una fila nueva empieza donde empezaba
    // una vieja (desplazada), el resto es igual: solo se desplaza.
    const std::size_t edited_end = start + inserted;
    std::size_t old_index = static_cast<std::size_t>(first);
    std::optional<std::size_t> same_from;
    layout_from(text, old[static_cast<std::size_t>(first)].start, width, rows,
                [&](std::size_t next) {
                    if (next < edited_end) {
                        return false;
                    }
                    while (old_index < old.size() && old[old_index].start + inserted < next + removed) {
                        ++old_index;
                    }
                    if (old_index < old.size() && old[old_index].start + inserted == next + removed) {
                        same_from = old_index;
                        return true;
                    }
                    return false;
                });
    if (same_from.has_value()) {
        rows.reserve(rows.size() + (old.size() - *same_from));
        for (std::size_t i = *same_from; i < old.size(); ++i) {
            TextRow row = old[i];
            row.start = row.start + inserted - removed;
            row.end = row.end + inserted - removed;
            rows.push_back(row);
        }
    }
    return rows;
}

std::size_t row_cursor_end(std::string_view text, const TextRow& row) {
    return row.hanging ? previous_glyph(text, row.end) : row.end;
}

RowColumn to_row_column(std::string_view text, const std::vector<TextRow>& rows,
                        TextCursor cursor) {
    if (rows.empty()) {
        return {};
    }
    const int row = row_containing(rows, cursor.offset);
    const auto index = static_cast<std::size_t>(row);
    if (cursor.affinity == Affinity::Up && row > 0 && rows[index].start == cursor.offset &&
        rows[index - 1].ends == RowBreak::Soft && !rows[index - 1].hanging) {
        return {row - 1, rows[index - 1].width};
    }
    int column = 0;
    const std::size_t end = std::min(cursor.offset, rows[index].end);
    for (std::size_t at = rows[index].start; at < end;) {
        const TextGlyph glyph = glyph_at(text, at);
        column += glyph.width;
        at = glyph.end;
    }
    return {row, column};
}

TextCursor from_row_column(std::string_view text, const std::vector<TextRow>& rows, int row,
                           int column, ColumnRule rule) {
    if (rows.empty()) {
        return {};
    }
    const TextRow& target =
        rows[static_cast<std::size_t>(std::clamp(row, 0, static_cast<int>(rows.size()) - 1))];
    // Con un espacio colgado, el final es antes del espacio (en su celda).
    const std::size_t end = row_cursor_end(text, target);
    const int end_column = target.hanging ? target.width - 1 : target.width;
    const TextCursor at_end{end, target.ends == RowBreak::Soft && !target.hanging
                                     ? Affinity::Up
                                     : Affinity::Down};
    if (column >= end_column) {
        return at_end;
    }
    column = std::max(0, column);
    int cell = 0;
    for (std::size_t at = target.start; at < end;) {
        const TextGlyph glyph = glyph_at(text, at);
        if (column < cell + glyph.width) {
            const bool after = rule == ColumnRule::Click && 2 * (column - cell) >= glyph.width;
            if (!after) {
                return {at, Affinity::Down};
            }
            return glyph.end == end ? at_end : TextCursor{glyph.end, Affinity::Down};
        }
        cell += glyph.width;
        at = glyph.end;
    }
    return at_end;
}

bool is_word_glyph(std::string_view text, std::size_t offset) {
    if (offset >= text.size()) {
        return false;
    }
    std::size_t next = offset;
    const std::optional<std::uint32_t> code = utf8::next_code_point(text, next);
    return code.has_value() && is_word_code(*code);
}

TextEdit::TextEdit(std::string text) : text_(std::move(text)), cursor_{text_.size()} {}

std::shared_ptr<const std::vector<TextRow>> TextEdit::rows(int width) const {
    for (const Cache& entry : cache_) {
        if (entry.width == width && entry.version == version_) {
            return entry.rows;
        }
    }
    auto rows = std::make_shared<const std::vector<TextRow>>(layout(text_, width));
    const auto same_width = std::find_if(cache_.begin(), cache_.end(),
                                         [width](const Cache& entry) { return entry.width == width; });
    if (same_width != cache_.end()) {
        cache_.erase(same_width);
    }
    if (cache_.size() >= kCacheEntries) {
        cache_.erase(cache_.begin());
    }
    cache_.push_back({width, version_, rows});
    return rows;
}

RowColumn TextEdit::cursor_position(int width) const {
    return to_row_column(text_, *rows(width), cursor_);
}

void TextEdit::set_text(std::string text) {
    set_text(std::move(text), std::string::npos);
}

void TextEdit::set_text(std::string text, std::size_t cursor) {
    text_ = std::move(text);
    ++version_; // Texto nuevo: la caché se descarta.
    cache_.clear();
    cursor_.offset = glyph_boundary_at_or_before(text_, cursor);
    edited();
}

void TextEdit::replace(std::size_t start, std::size_t removed, std::string_view piece) {
    text_.replace(start, removed, piece);
    ++version_;
    for (auto entry = cache_.begin(); entry != cache_.end();) {
        if (entry->version + 1 != version_) {
            entry = cache_.erase(entry);
            continue;
        }
        entry->rows = std::make_shared<const std::vector<TextRow>>(
            relayout(*entry->rows, text_, entry->width, start, removed, piece.size()));
        entry->version = version_;
        ++entry;
    }
}

void TextEdit::insert(std::string_view piece) {
    if (piece.empty()) {
        return;
    }
    replace(cursor_.offset, 0, piece);
    // Si lo insertado se juntó con lo que sigue (un combinante después), el
    // cursor va al final de ese glifo.
    cursor_.offset = glyph_boundary_at_or_after(text_, cursor_.offset + piece.size());
    edited();
}

void TextEdit::type(std::string_view piece) {
    std::size_t removed = 0;
    if (overwrite_ && cursor_.offset < text_.size() && text_[cursor_.offset] != '\n') {
        removed = next_glyph(text_, cursor_.offset) - cursor_.offset;
    }
    if (piece.empty() && removed == 0) {
        return;
    }
    replace(cursor_.offset, removed, piece);
    cursor_.offset = glyph_boundary_at_or_after(text_, cursor_.offset + piece.size());
    edited();
}

bool TextEdit::erase_before() {
    if (cursor_.offset == 0) {
        return false;
    }
    const std::size_t start = previous_glyph(text_, cursor_.offset);
    replace(start, cursor_.offset - start, {});
    cursor_.offset = glyph_boundary_at_or_before(text_, start);
    edited();
    return true;
}

bool TextEdit::erase_after() {
    if (cursor_.offset >= text_.size()) {
        return false;
    }
    replace(cursor_.offset, next_glyph(text_, cursor_.offset) - cursor_.offset, {});
    cursor_.offset = glyph_boundary_at_or_before(text_, cursor_.offset);
    edited();
    return true;
}

bool TextEdit::move_left() {
    if (cursor_.offset == 0) {
        return false;
    }
    cursor_.offset = previous_glyph(text_, cursor_.offset);
    moved_horizontally();
    return true;
}

bool TextEdit::move_right() {
    if (cursor_.offset >= text_.size()) {
        return false;
    }
    cursor_.offset = next_glyph(text_, cursor_.offset);
    moved_horizontally();
    return true;
}

bool TextEdit::move_word_left() {
    if (cursor_.offset == 0) {
        return false;
    }
    std::size_t at = cursor_.offset;
    // Lo que no es palabra, y luego la palabra (ftxui::Input::HandleLeftCtrl).
    while (at > 0) {
        const std::size_t previous = previous_glyph(text_, at);
        if (is_word_glyph(text_, previous)) {
            break;
        }
        at = previous;
    }
    while (at > 0) {
        const std::size_t previous = previous_glyph(text_, at);
        if (!is_word_glyph(text_, previous)) {
            break;
        }
        at = previous;
    }
    cursor_.offset = at;
    moved_horizontally();
    return true;
}

bool TextEdit::move_word_right() {
    if (cursor_.offset >= text_.size()) {
        return false;
    }
    std::size_t at = cursor_.offset;
    // Hasta entrar a una palabra, y luego hasta su final
    // (ftxui::Input::HandleRightCtrl).
    while (at < text_.size()) {
        at = next_glyph(text_, at);
        if (is_word_glyph(text_, at)) {
            break;
        }
    }
    while (at < text_.size()) {
        if (!is_word_glyph(text_, at)) {
            break;
        }
        at = next_glyph(text_, at);
    }
    cursor_.offset = at;
    moved_horizontally();
    return true;
}

void TextEdit::move_to_row(int width, int row) {
    const std::shared_ptr<const std::vector<TextRow>> all = rows(width);
    const int column = desired_column_.value_or(to_row_column(text_, *all, cursor_).column);
    cursor_ = from_row_column(text_, *all, row, column, ColumnRule::Floor);
    desired_column_ = column;
    reveal_ = true;
}

bool TextEdit::move_up(int width) {
    const int row = cursor_position(width).row;
    if (row > 0) {
        move_to_row(width, row - 1);
        return true;
    }
    // Primera fila: al principio del texto, como ftxui::Input en la primera
    // línea. La columna deseada se conserva para volver a bajar.
    if (cursor_.offset == 0) {
        return false;
    }
    if (!desired_column_.has_value()) {
        desired_column_ = cursor_position(width).column;
    }
    cursor_ = {0, Affinity::Down};
    reveal_ = true;
    return true;
}

bool TextEdit::move_down(int width) {
    const int row = cursor_position(width).row;
    const int last = static_cast<int>(rows(width)->size()) - 1;
    if (row < last) {
        move_to_row(width, row + 1);
        return true;
    }
    // Última fila: al final del texto, como ftxui::Input en la última línea.
    if (cursor_.offset >= text_.size()) {
        return false;
    }
    if (!desired_column_.has_value()) {
        desired_column_ = cursor_position(width).column;
    }
    cursor_ = {text_.size(), Affinity::Down};
    reveal_ = true;
    return true;
}

bool TextEdit::move_home(int width) {
    const std::shared_ptr<const std::vector<TextRow>> all = rows(width);
    const TextRow& row =
        (*all)[static_cast<std::size_t>(to_row_column(text_, *all, cursor_).row)];
    cursor_.offset = row.start;
    moved_horizontally();
    return true;
}

bool TextEdit::move_end(int width) {
    const std::shared_ptr<const std::vector<TextRow>> all = rows(width);
    const TextRow& row =
        (*all)[static_cast<std::size_t>(to_row_column(text_, *all, cursor_).row)];
    cursor_.offset = row_cursor_end(text_, row);
    moved_horizontally();
    // En una fila cortada, el final se queda arriba (en la columna reservada);
    // con un espacio colgado, antes del espacio (en su celda).
    cursor_.affinity =
        row.ends == RowBreak::Soft && !row.hanging ? Affinity::Up : Affinity::Down;
    return true;
}

bool TextEdit::page(int direction, int width, int height) {
    const int step = std::max(1, height);
    const std::shared_ptr<const std::vector<TextRow>> all = rows(width);
    const int count = static_cast<int>(all->size());
    const int row = to_row_column(text_, *all, cursor_).row;
    const int target = std::clamp(row + (direction < 0 ? -step : step), 0, count - 1);
    if (target == row) {
        return direction < 0 ? move_up(width) : move_down(width);
    }
    // La vista se mueve lo mismo que el cursor: la fila del cursor queda en
    // el mismo lugar del campo (salvo en los extremos).
    const int top = top_row(width, height);
    move_to_row(width, target);
    const int moved =
        std::clamp(top + (direction < 0 ? -step : step), 0, std::max(0, count - step));
    top_offset_ = (*all)[static_cast<std::size_t>(moved)].start;
    return true;
}

void TextEdit::click(int row, int column, int width) {
    cursor_ = from_row_column(text_, *rows(width), row, column, ColumnRule::Click);
    desired_column_.reset();
    reveal_ = true;
}

int TextEdit::peek_top_row(int width, int height) const {
    const std::shared_ptr<const std::vector<TextRow>> all = rows(width);
    const int count = static_cast<int>(all->size());
    const int visible = std::max(1, height);
    int top = row_containing(*all, top_offset_);
    if (reveal_ || width != view_width_ || height != view_height_) {
        const int cursor_row = to_row_column(text_, *all, cursor_).row;
        if (cursor_row < top) {
            top = cursor_row;
        } else if (cursor_row >= top + visible) {
            top = cursor_row - visible + 1;
        }
    }
    return std::clamp(top, 0, std::max(0, count - visible));
}

int TextEdit::top_row(int width, int height) {
    const int top = peek_top_row(width, height);
    reveal_ = false;
    view_width_ = width;
    view_height_ = height;
    top_offset_ = (*rows(width))[static_cast<std::size_t>(top)].start;
    return top;
}

bool TextEdit::scroll(int delta, int width, int height) {
    const int top = top_row(width, height);
    const std::shared_ptr<const std::vector<TextRow>> all = rows(width);
    const int count = static_cast<int>(all->size());
    const int moved = std::clamp(top + delta, 0, std::max(0, count - std::max(1, height)));
    if (moved == top) {
        return false;
    }
    top_offset_ = (*all)[static_cast<std::size_t>(moved)].start;
    return true;
}

void TextEdit::edited() {
    desired_column_.reset();
    cursor_.affinity = Affinity::Down;
    reveal_ = true;
}

void TextEdit::moved_horizontally() {
    desired_column_.reset();
    cursor_.affinity = Affinity::Down;
    reveal_ = true;
}

} // namespace chatbot::cli
