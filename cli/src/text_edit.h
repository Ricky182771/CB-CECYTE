#ifndef CHATBOT_CLI_TEXT_EDIT_H
#define CHATBOT_CLI_TEXT_EDIT_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace chatbot::cli {

// Modelo de los campos de varias líneas con ajuste de línea (TextArea): el
// texto, el cursor, la distribución en filas y la vista. Sin componentes ni
// terminal; los anchos salen de ftxui::string_width (la misma tabla con la
// que FTXUI dibuja), así que el modelo y el dibujo siempre coinciden.
//
// Glifo: lo que el cursor salta de una vez. Un código UTF-8 con los
// combinantes que lo siguen (string_width 0 y no es control), un "\n", un
// "\t" (4 celdas), un control o un byte inválido (1 celda, se dibuja como
// U+FFFD) o un combinante sin base al principio de una línea (1 celda, se
// dibuja sobre un espacio). El texto nunca se toca: lo que se edita es lo que
// se envía.

/// Cómo termina una fila en pantalla.
enum class RowBreak : std::uint8_t {
    Newline, ///< Un "\n": está en end y no ocupa celda.
    Soft,    ///< Corte suave: la fila siguiente empieza en end.
    End,     ///< Final del texto.
};

/// Una fila en pantalla: los bytes [start, end) del texto.
struct TextRow {
    std::size_t start = 0;
    std::size_t end = 0;
    int width = 0; ///< Celdas (con el espacio colgado, si hay).
    RowBreak ends = RowBreak::End;
    /// Termina en un espacio que ya no cabía en el ancho útil y quedó en la
    /// columna reservada del cursor (no se ve). Siempre es un corte suave; el
    /// cursor nunca se dibuja después de ese espacio en esta fila.
    bool hanging = false;

    bool operator==(const TextRow&) const = default;
};

/// Tipo de glifo (cómo se dibuja).
enum class GlyphKind : std::uint8_t {
    Normal,      ///< Los bytes tal cual, en width celdas (1 o 2).
    Newline,     ///< "\n": termina la fila, 0 celdas.
    Tab,         ///< "\t": kTabWidth espacios.
    Replacement, ///< Byte inválido o control: U+FFFD, 1 celda.
    Baseless,    ///< Combinante sin base: un espacio con el combinante, 1 celda.
};

/// Celdas de un "\t" (un solo glifo).
inline constexpr int kTabWidth = 4;

/// Un glifo del texto: los bytes [start, end).
struct TextGlyph {
    std::size_t start = 0;
    std::size_t end = 0;
    int width = 0; ///< Celdas.
    GlyphKind kind = GlyphKind::Normal;
    /// Bytes del código base (sin los combinantes): [start, base_end).
    std::size_t base_end = 0;
};

/// El glifo que empieza en offset (offset < text.size() y en un límite).
[[nodiscard]] TextGlyph glyph_at(std::string_view text, std::size_t offset);
/// El límite de glifo siguiente (text.size() al final).
[[nodiscard]] std::size_t next_glyph(std::string_view text, std::size_t offset);
/// El límite de glifo anterior (0 al principio).
[[nodiscard]] std::size_t previous_glyph(std::string_view text, std::size_t offset);
/// El mayor límite de glifo que no pasa de offset (offset se acota al texto).
[[nodiscard]] std::size_t glyph_boundary_at_or_before(std::string_view text, std::size_t offset);
/// Lo que se dibuja en la primera celda del glifo (las demás van vacías, o
/// con espacios si es un "\t").
[[nodiscard]] std::string glyph_display(std::string_view text, const TextGlyph& glyph);
/// Lo que se dibuja en la celda cell (de 0 a glyph.width - 1) del glifo: la
/// primera, glyph_display; las de un "\t", espacios; la segunda de un glifo
/// ancho, nada (como ftxui::text).
[[nodiscard]] std::string glyph_cell(std::string_view text, const TextGlyph& glyph, int cell);

/// Distribuye el texto en filas de width celdas: ajuste por palabras al
/// ancho útil (width - 1; la última columna es del cursor al final de una
/// fila). Se corta después del último espacio que cabe; un espacio que ya no
/// cabe se queda al final de la fila, en la columna reservada (hanging; solo
/// ese: si siguen más espacios, bajan), y si era lo último de la línea, le
/// sigue una fila vacía para el cursor. Una palabra más larga que la fila se
/// parte por glifos y un glifo que no cabe pasa completo a la fila siguiente
/// (también un "\t", que no cabe en la columna reservada). Con width < 2, una
/// fila por glifo. Siempre hay al menos una fila, y un "\n" al final deja una
/// fila vacía después.
[[nodiscard]] std::vector<TextRow> layout(std::string_view text, int width);

/// Lo mismo que layout(text, width), a partir de las filas old del texto de
/// antes de reemplazar removed bytes en start por inserted bytes: solo se
/// distribuye de nuevo desde la fila de la edición hasta que las filas
/// vuelven a coincidir; las de después solo se desplazan.
[[nodiscard]] std::vector<TextRow> relayout(const std::vector<TextRow>& old, std::string_view text,
                                            int width, std::size_t start, std::size_t removed,
                                            std::size_t inserted);

/// Dónde se dibuja el cursor cuando su desplazamiento cae justo en un corte
/// suave: al principio de la fila de abajo (Down) o al final de la de arriba
/// (Up). En cualquier otro lugar no importa, y después de un espacio colgado
/// (TextRow::hanging) siempre va abajo.
enum class Affinity : std::uint8_t { Down, Up };

/// El cursor: un desplazamiento en bytes, siempre en un límite de glifo.
struct TextCursor {
    std::size_t offset = 0;
    Affinity affinity = Affinity::Down;

    bool operator==(const TextCursor&) const = default;
};

/// Fila (índice en rows) y columna (en celdas desde el principio de la fila).
struct RowColumn {
    int row = 0;
    int column = 0;

    bool operator==(const RowColumn&) const = default;
};

/// Hasta dónde llega el cursor en la fila: su final, o antes del espacio
/// colgado.
[[nodiscard]] std::size_t row_cursor_end(std::string_view text, const TextRow& row);

/// Fila y columna del cursor en rows (de layout(text, ...)).
[[nodiscard]] RowColumn to_row_column(std::string_view text, const std::vector<TextRow>& rows,
                                      TextCursor cursor);

/// Cómo se elige el límite cuando la columna cae a la mitad de un glifo.
enum class ColumnRule : std::uint8_t {
    Floor, ///< Al límite anterior (↑/↓ con la columna deseada).
    Click, ///< En la mitad derecha, después del glifo (clic).
};

/// El cursor en (row, column). Una columna en el final de la fila o más
/// allá va al final (row_cursor_end), con afinidad Up si la fila termina en
/// un corte suave sin espacio colgado. row y column se acotan.
[[nodiscard]] TextCursor from_row_column(std::string_view text, const std::vector<TextRow>& rows,
                                         int row, int column, ColumnRule rule);

/// true si el glifo cuenta como parte de una palabra para Ctrl+←/→: letras y
/// dígitos (también con acento); no los espacios, la puntuación (¿ ¡ « » _
/// incluidos) ni los símbolos.
[[nodiscard]] bool is_word_glyph(std::string_view text, std::size_t offset);

/// Texto editable con cursor, distribución en caché y vista propia. Las
/// funciones de movimiento devuelven lo mismo que ftxui::Input en v7.0.3 (si
/// devuelven false el evento sigue a los contenedores).
class TextEdit {
public:
    TextEdit() = default;
    /// El cursor al final.
    explicit TextEdit(std::string text);

    [[nodiscard]] const std::string& text() const { return text_; }
    /// Cambia con cada edición (para la caché de quien dibuja).
    [[nodiscard]] std::uint64_t version() const { return version_; }
    [[nodiscard]] TextCursor cursor() const { return cursor_; }
    /// Modo de sobrescritura (Insert): escribir reemplaza el glifo siguiente.
    [[nodiscard]] bool overwrite() const { return overwrite_; }
    void toggle_overwrite() { overwrite_ = !overwrite_; }

    /// Filas con ese ancho (en caché por versión y ancho). Compartidas: siguen
    /// válidas aunque después se edite o se pidan otros anchos.
    [[nodiscard]] std::shared_ptr<const std::vector<TextRow>> rows(int width) const;
    /// Fila y columna del cursor con ese ancho.
    [[nodiscard]] RowColumn cursor_position(int width) const;

    /// Reemplaza el texto: el cursor al final, con afinidad Down, y la vista
    /// lo sigue.
    void set_text(std::string text);
    /// Igual, con el cursor en cursor (acotado y llevado a un límite de glifo).
    void set_text(std::string text, std::size_t cursor);

    /// Inserta piece en el cursor (sin sobrescribir) y deja el cursor después.
    void insert(std::string_view piece);
    /// Lo que escribe una tecla o un pegado: con sobrescritura, primero borra
    /// el glifo siguiente (salvo un "\n" o el final), como ftxui::Input.
    void type(std::string_view piece);
    /// Retroceso: borra el glifo anterior. false al principio.
    bool erase_before();
    /// Supr: borra el glifo siguiente. false al final.
    bool erase_after();

    bool move_left();
    bool move_right();
    /// Ctrl+←/→: como ftxui::Input (salta lo que no es palabra y luego la palabra).
    bool move_word_left();
    bool move_word_right();
    /// ↑/↓ por fila en pantalla con la columna deseada. En la primera fila, ↑
    /// va al principio del texto (false si ya estaba); en la última, ↓ va al
    /// final (false si ya estaba).
    bool move_up(int width);
    bool move_down(int width);
    /// Inicio/Fin de la fila en pantalla. Siempre true.
    bool move_home(int width);
    bool move_end(int width);
    /// PgUp (direction < 0) o PgDn: el cursor y la vista, height filas. En la
    /// primera o la última fila, como ↑/↓.
    bool page(int direction, int width, int height);
    /// Clic en (row, column) de rows(width): row y column se acotan.
    void click(int row, int column, int width);

    /// Primera fila visible para ese ancho y alto. Después de un movimiento o
    /// una edición, o si cambió el ancho o el alto, la ajusta lo mínimo para
    /// que se vea el cursor; si no (la rueda la movió), la respeta. Siempre
    /// dentro de [0, filas - alto].
    [[nodiscard]] int top_row(int width, int height);
    /// Lo que devolvería top_row, sin cambiar la vista (para medir antes de
    /// dibujar).
    [[nodiscard]] int peek_top_row(int width, int height) const;
    /// Rueda: mueve la vista delta filas sin mover el cursor. true si se movió.
    bool scroll(int delta, int width, int height);

private:
    /// Reemplaza removed bytes en start por piece y pone al día la caché (con
    /// relayout). No toca el cursor.
    void replace(std::size_t start, std::size_t removed, std::string_view piece);
    /// Después de editar: columna deseada olvidada, afinidad Down y la vista
    /// sigue al cursor.
    void edited();
    /// Después de un movimiento horizontal.
    void moved_horizontally();
    void move_to_row(int width, int row);

    struct Cache {
        int width = 0;
        std::uint64_t version = 0;
        std::shared_ptr<const std::vector<TextRow>> rows;
    };

    std::string text_;
    std::uint64_t version_ = 0;
    TextCursor cursor_;
    bool overwrite_ = false;
    std::optional<int> desired_column_; ///< ↑/↓ la conservan; lo demás la olvida.
    mutable std::vector<Cache> cache_;  ///< Pocas entradas: los anchos recientes.

    // Vista: la primera fila visible se guarda por su primer byte, para que
    // un cambio de ancho conserve el lugar del texto y no el número de fila.
    std::size_t top_offset_ = 0;
    bool reveal_ = true; ///< La siguiente top_row deja visible el cursor.
    int view_width_ = -1;
    int view_height_ = -1;
};

} // namespace chatbot::cli

#endif // CHATBOT_CLI_TEXT_EDIT_H
