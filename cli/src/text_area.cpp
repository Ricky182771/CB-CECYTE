#include "text_area.h"

#include "input_edit.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/box.hpp>
#include <ftxui/screen/screen.hpp>

#include <algorithm>
#include <optional>
#include <utility>
#include <vector>

namespace chatbot::cli {

struct TextArea::State {
    explicit State(TextAreaOption chosen) : option(std::move(chosen)) {}

    TextAreaOption option;
    TextEdit edit;
    // Lo que midió el último cuadro (antes del primero, un ancho cualquiera).
    int width = 80;      ///< Ancho del campo.
    int text_width = 80; ///< Ancho del texto: sin la columna de la barra.
    int height = 1;
    int top = 0;      ///< Primera fila visible.
    bool bar = false; ///< El texto no cabe: barra en la última columna.
    ftxui::Box box{0, -1, 0, -1};

    /// Alto que pide el campo con width columnas: sus filas entre el mínimo y
    /// el máximo; sin máximo, el mínimo (y crece con flex).
    [[nodiscard]] int wanted_height(int columns) const {
        const int least = std::max(1, option.min_rows);
        if (option.max_rows <= 0) {
            return least;
        }
        const int rows = static_cast<int>(edit.rows(columns)->size());
        return std::clamp(rows, least, std::max(least, option.max_rows));
    }

    /// Con la barra, el texto pierde una columna. Solo si no cabe con el
    /// ancho completo: con menos ancho nunca hay menos filas, así que no
    /// oscila.
    [[nodiscard]] bool needs_bar(int columns, int rows_visible) const {
        return columns > 2 && static_cast<int>(edit.rows(columns)->size()) > rows_visible;
    }
};

namespace {

/// El dibujo de un TextArea. La altura depende del ancho, que se sabe hasta
/// SetBox: como flexbox en FTXUI v7.0.3, si el ancho que recibe no es el que
/// supuso en ComputeRequirement (el del cuadro anterior), pide otra pasada
/// del layout (Node::Check). Todo pasa antes de dibujar: no hay parpadeo.
/// El hijo es el elemento del cursor (focus o focusCursor*, como
/// ftxui::Input): no se dibuja, solo se coloca en su celda para que FTXUI
/// ponga ahí el cursor real de la terminal.
class TextAreaNode : public ftxui::Node {
public:
    TextAreaNode(std::shared_ptr<TextArea::State> state, ftxui::Element cursor)
        : ftxui::Node(ftxui::Elements{std::move(cursor)}), state_(std::move(state)) {}

    void ComputeRequirement() override {
        children_[0]->ComputeRequirement();
        need_iteration_ = false;
        const TextArea::State& s = *state_;
        // Se supone el ancho y el alto que dio el cuadro anterior (no el que
        // se pide: si el padre lo encoge, el alto real es otro y la vista se
        // calcularía con uno distinto al de SetBox). min_y solo depende del
        // ancho, así que la segunda pasada recibe el mismo alto y ya no pide
        // otra.
        assumed_width_ = s.width;
        assumed_height_ = s.height;
        requirement_ = ftxui::Requirement{};
        requirement_.min_x = 1;
        requirement_.flex_grow_x = 1;
        requirement_.flex_shrink_x = 1;
        requirement_.min_y = s.wanted_height(s.width);
        if (s.option.max_rows <= 0) {
            requirement_.flex_grow_y = 1;
            requirement_.flex_shrink_y = 1; // Los botones de abajo nunca se tapan.
        }
        // El cursor, con lo que se supone del cuadro (sin cambiar la vista).
        requirement_.focused = children_[0]->requirement().focused;
        const std::optional<ftxui::Box> cell = cursor_cell(assumed_width_, assumed_height_, false);
        if (cell.has_value()) {
            requirement_.focused.box = *cell;
        } else {
            requirement_.focused.enabled = false; // La rueda lo dejó fuera de la vista.
        }
        assumed_visible_ = cell.has_value();
    }

    void SetBox(ftxui::Box box) override {
        ftxui::Node::SetBox(box);
        TextArea::State& s = *state_;
        const int width = box.x_max - box.x_min + 1;
        const int height = box.y_max - box.y_min + 1;
        s.box = box;
        if (width <= 0 || height <= 0) {
            children_[0]->SetBox(ftxui::Box{box.x_min, box.x_min - 1, box.y_min, box.y_min - 1});
            return;
        }
        need_iteration_ = width != assumed_width_ || height != assumed_height_;
        s.width = width;
        s.height = height;
        s.bar = s.needs_bar(width, height);
        s.text_width = s.bar ? width - 1 : width;
        s.top = s.edit.top_row(s.text_width, height);
        const std::optional<ftxui::Box> cell = cursor_cell(width, height, true);
        need_iteration_ = need_iteration_ || cell.has_value() != assumed_visible_;
        if (cell.has_value()) {
            children_[0]->SetBox(ftxui::Box{box.x_min + cell->x_min, box.x_min + cell->x_min,
                                            box.y_min + cell->y_min, box.y_min + cell->y_min});
        } else {
            children_[0]->SetBox(ftxui::Box{box.x_min, box.x_min, box.y_min, box.y_min});
        }
    }

    void Check(Status* status) override {
        ftxui::Node::Check(status);
        status->need_iteration |= need_iteration_;
    }

    void Render(ftxui::Screen& screen) override {
        const TextArea::State& s = *state_;
        const ftxui::Box visible = ftxui::Box::Intersection(screen.stencil, box_);
        if (visible.IsEmpty()) {
            return;
        }
        const std::string& text = s.edit.text();
        if (text.empty()) {
            render_placeholder(screen);
            return;
        }
        const std::shared_ptr<const std::vector<TextRow>> rows = s.edit.rows(s.text_width);
        const int last_column = box_.x_min + s.text_width - 1;
        for (int y = 0; y < s.height; ++y) {
            const auto index = static_cast<std::size_t>(s.top + y);
            if (index >= rows->size()) {
                break;
            }
            const TextRow& row = (*rows)[index];
            int x = box_.x_min;
            for (std::size_t at = row.start; at < row.end;) {
                const TextGlyph glyph = glyph_at(text, at);
                at = glyph.end;
                if (x + glyph.width - 1 > last_column && x > box_.x_min) {
                    break; // Solo pasa con un glifo más ancho que el campo.
                }
                for (int cell = 0; cell < glyph.width; ++cell) {
                    const int cx = x + cell;
                    const int cy = box_.y_min + y;
                    if (!visible.Contain(cx, cy) || cx > last_column) {
                        continue;
                    }
                    // Un "\t" son espacios; un glifo ancho deja vacía su
                    // segunda celda, como ftxui::text.
                    screen.CellAt(cx, cy).character = glyph_cell(text, glyph, cell);
                }
                x += glyph.width;
            }
        }
        if (s.bar) {
            render_bar(screen, static_cast<int>(rows->size()));
        }
    }

private:
    /// Celda del cursor relativa al campo, o nullopt si la vista no lo
    /// muestra. commit: después de SetBox (la vista ya quedó fija).
    [[nodiscard]] std::optional<ftxui::Box> cursor_cell(int width, int height, bool commit) const {
        const TextArea::State& s = *state_;
        if (width <= 0 || height <= 0) {
            return std::nullopt;
        }
        if (s.edit.text().empty()) {
            return ftxui::Box{0, 0, 0, 0}; // Al principio del placeholder.
        }
        const int text_width = s.needs_bar(width, height) ? width - 1 : width;
        const int top = commit ? s.top : s.edit.peek_top_row(text_width, height);
        const RowColumn at = s.edit.cursor_position(text_width);
        if (at.row < top || at.row >= top + height) {
            return std::nullopt;
        }
        const int column = std::min(at.column, std::max(0, text_width - 1));
        return ftxui::Box{column, column, at.row - top, at.row - top};
    }

    void render_placeholder(ftxui::Screen& screen) const {
        const TextArea::State& s = *state_;
        if (!s.option.placeholder || s.option.palette == nullptr) {
            return;
        }
        ftxui::Element shown = ftxui::text(s.option.placeholder(s.width)) |
                               s.option.palette->ink(&Theme::input_placeholder);
        shown->ComputeRequirement();
        shown->SetBox(ftxui::Box{box_.x_min, box_.x_max, box_.y_min, box_.y_min});
        shown->Render(screen);
    }

    /// La barra del historial (Palette::vscroll: su color y, en modo
    /// Sencillos, los respaldos de glyph_table) en la última columna. Se le da
    /// una caja con el alto de todas las filas, corrida según la primera
    /// visible, y la columna de la barra como stencil: así calcula el tamaño y
    /// el lugar del indicador igual que dentro de un frame.
    void render_bar(ftxui::Screen& screen, int rows) const {
        const TextArea::State& s = *state_;
        if (s.option.palette == nullptr) {
            return;
        }
        const ftxui::Box column{box_.x_max, box_.x_max, box_.y_min, box_.y_max};
        const ftxui::Box stencil = ftxui::Box::Intersection(screen.stencil, column);
        if (stencil.IsEmpty()) {
            return;
        }
        ftxui::Element bar = s.option.palette->vscroll(ftxui::emptyElement());
        // rows + 1 filas, como le da frame a su hijo (frame.cpp): con una fila
        // de más que la vista, vscroll_indicator ya dibuja el indicador.
        bar->SetBox(ftxui::Box{box_.x_max, box_.x_max, box_.y_min - s.top,
                               box_.y_min - s.top + rows});
        const ftxui::Box saved = screen.stencil;
        screen.stencil = stencil;
        bar->Render(screen);
        screen.stencil = saved;
    }

    std::shared_ptr<TextArea::State> state_;
    int assumed_width_ = 0;
    int assumed_height_ = 0;
    bool assumed_visible_ = true;
    bool need_iteration_ = false;
};

} // namespace

TextArea::TextArea(TextAreaOption option) : state_(std::make_shared<State>(std::move(option))) {}

const std::string& TextArea::text() const { return state_->edit.text(); }

void TextArea::set_text(std::string text) { state_->edit.set_text(std::move(text)); }

void TextArea::set_text(std::string text, std::size_t cursor) {
    state_->edit.set_text(std::move(text), cursor);
}

void TextArea::insert(std::string_view piece) {
    if (piece.empty()) {
        return;
    }
    state_->edit.insert(piece);
    changed();
}

bool TextArea::backslash_newline() {
    std::string text = state_->edit.text();
    int cursor = static_cast<int>(state_->edit.cursor().offset);
    if (!chatbot::cli::backslash_newline(text, cursor)) {
        return false;
    }
    state_->edit.set_text(std::move(text), static_cast<std::size_t>(cursor));
    return true;
}

std::size_t TextArea::cursor() const { return state_->edit.cursor().offset; }

int TextArea::row_count() const {
    return static_cast<int>(state_->edit.rows(state_->text_width)->size());
}

void TextArea::set_max_rows(int rows) { state_->option.max_rows = rows; }

void TextArea::set_on_enter(std::function<void()> on_enter) {
    state_->option.on_enter = std::move(on_enter);
}

bool TextArea::contains(int x, int y) const { return state_->box.Contain(x, y); }

bool TextArea::scrollable() const { return state_->bar; }

const TextEdit& TextArea::edit() const { return state_->edit; }

ftxui::Element TextArea::OnRender() {
    // Como ftxui::Input: con el foco (o el ratón encima), el cursor real de
    // la terminal, barra al insertar y bloque al sobrescribir; si no, solo
    // focus (sin cursor visible).
    const bool focused = Focused();
    ftxui::Element cursor = ftxui::text(" ");
    if (!focused && !hovered_) {
        cursor = ftxui::focus(std::move(cursor));
    } else if (state_->edit.overwrite()) {
        cursor = ftxui::focusCursorBlockBlinking(std::move(cursor));
    } else {
        cursor = ftxui::focusCursorBarBlinking(std::move(cursor));
    }
    return std::make_shared<TextAreaNode>(state_, std::move(cursor));
}

bool TextArea::OnEvent(ftxui::Event event) {
    if (event.is_mouse()) {
        return handle_mouse(std::move(event));
    }
    if (!Focused()) {
        return false;
    }
    return handle_key(event);
}

bool TextArea::handle_mouse(ftxui::Event event) { // mouse() no es const.
    const ftxui::Mouse& mouse = event.mouse();
    State& s = *state_;
    hovered_ = s.box.Contain(mouse.x, mouse.y) && CaptureMouse(event);
    if (!hovered_) {
        return false;
    }
    if (mouse.button == ftxui::Mouse::WheelUp || mouse.button == ftxui::Mouse::WheelDown) {
        if (!s.bar) {
            return false; // Todo cabe: la rueda es de quien contiene el campo.
        }
        (void)s.edit.scroll(mouse.button == ftxui::Mouse::WheelUp ? -kWheelStep : kWheelStep,
                            s.text_width, s.height);
        return true;
    }
    if (mouse.button != ftxui::Mouse::Left || mouse.motion != ftxui::Mouse::Pressed) {
        return false;
    }
    TakeFocus();
    // Las filas que se ven son las del último cuadro (s.top).
    s.edit.click(s.top + mouse.y - s.box.y_min, mouse.x - s.box.x_min, s.text_width);
    return true;
}

bool TextArea::handle_key(const ftxui::Event& event) {
    TextEdit& edit = state_->edit;
    const int width = state_->text_width;
    const int height = state_->height;
    if (event == ftxui::Event::Tab || event == ftxui::Event::TabReverse) {
        return false; // Nunca insertan: mueven el foco.
    }
    if (event == ftxui::Event::Return) {
        if (state_->option.enter == EnterKey::Newline) {
            edit.type("\n");
            changed();
        }
        if (state_->option.on_enter) {
            state_->option.on_enter(); // Al último: puede cambiar el texto.
        }
        return true;
    }
    if (event.is_character()) {
        edit.type(event.character());
        changed();
        return true;
    }
    if (event == ftxui::Event::Backspace || event == ftxui::Event::Delete) {
        if (!(event == ftxui::Event::Backspace ? edit.erase_before() : edit.erase_after())) {
            return false;
        }
        changed();
        return true;
    }
    if (event == ftxui::Event::Insert) {
        edit.toggle_overwrite();
        return true;
    }
    using Move = bool (*)(TextEdit&, int, int);
    const std::pair<const ftxui::Event*, Move> moves[] = {
        {&ftxui::Event::ArrowLeft, [](TextEdit& e, int, int) { return e.move_left(); }},
        {&ftxui::Event::ArrowRight, [](TextEdit& e, int, int) { return e.move_right(); }},
        {&ftxui::Event::ArrowUp, [](TextEdit& e, int w, int) { return e.move_up(w); }},
        {&ftxui::Event::ArrowDown, [](TextEdit& e, int w, int) { return e.move_down(w); }},
        {&ftxui::Event::Home, [](TextEdit& e, int w, int) { return e.move_home(w); }},
        {&ftxui::Event::End, [](TextEdit& e, int w, int) { return e.move_end(w); }},
        {&ftxui::Event::ArrowLeftCtrl, [](TextEdit& e, int, int) { return e.move_word_left(); }},
        {&ftxui::Event::ArrowRightCtrl, [](TextEdit& e, int, int) { return e.move_word_right(); }},
        {&ftxui::Event::PageUp, [](TextEdit& e, int w, int h) { return e.page(-1, w, h); }},
        {&ftxui::Event::PageDown, [](TextEdit& e, int w, int h) { return e.page(1, w, h); }},
    };
    for (const auto& [key, move] : moves) {
        if (event == *key) {
            return move(edit, width, height);
        }
    }
    return false;
}

void TextArea::changed() {
    if (state_->option.on_change) {
        state_->option.on_change();
    }
}

std::shared_ptr<TextArea> make_text_area(TextAreaOption option) {
    return ftxui::Make<TextArea>(std::move(option));
}

} // namespace chatbot::cli
