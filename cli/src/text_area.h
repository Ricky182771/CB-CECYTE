#ifndef CHATBOT_CLI_TEXT_AREA_H
#define CHATBOT_CLI_TEXT_AREA_H

#include "text_edit.h"
#include "theme.h"

#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace chatbot::cli {

/// Qué hace Enter en un TextArea.
enum class EnterKey {
    Submit,  ///< Llama a on_enter (la caja de la conversación: enviar).
    Newline, ///< Inserta "\n" y luego llama a on_enter si hay (como
             ///< InputOption::multiline en ftxui::Input).
};

struct TextAreaOption {
    /// Con qué se dibuja (placeholder y barra). Debe vivir más que el campo.
    const Palette* palette = nullptr;
    /// Alto mínimo en filas.
    int min_rows = 1;
    /// Alto máximo: el campo crece con sus filas en pantalla hasta aquí y
    /// después se desplaza. 0: ocupa el alto que le den (flex), al menos
    /// min_rows, y puede encogerse si no cabe.
    int max_rows = 0;
    EnterKey enter = EnterKey::Newline;
    std::function<void()> on_enter;
    /// Después de cada cambio del texto por una tecla o un pegado (una vez
    /// por evento). set_text no lo llama.
    std::function<void()> on_change;
    /// Placeholder con el texto vacío, según las columnas del campo (la
    /// versión corta en pantallas angostas). Vacío: sin placeholder.
    std::function<std::string(int columns)> placeholder;
};

/// Campo de texto de varias líneas con ajuste por palabras (text_edit.h):
/// reemplaza a ftxui::Input donde el texto puede ser largo (la caja de la
/// conversación e "Instrucciones del sistema"). Conserva lo que hace
/// ftxui::Input en v7.0.3 (teclas, cursor real de la terminal con
/// focusCursorBarBlinking, Insert, el pegado como un solo Event::Character,
/// clic y foco) salvo que Inicio/Fin y ↑/↓ van por fila en pantalla. El
/// texto, el cursor y la vista son suyos. Se usa solo desde el hilo de la
/// interfaz.
class TextArea : public ftxui::ComponentBase {
public:
    explicit TextArea(TextAreaOption option);

    [[nodiscard]] const std::string& text() const;
    /// Pone el texto con el cursor al final (afinidad abajo) y la vista en
    /// el cursor. No llama a on_change.
    void set_text(std::string text);
    /// Igual, con el cursor en cursor (bytes; se acota a un límite de glifo).
    void set_text(std::string text, std::size_t cursor);
    /// Inserta en el cursor (sin sobrescribir) y llama a on_change.
    void insert(std::string_view piece);
    /// "\" + Enter (input_edit.h): si justo antes del cursor hay "\", lo
    /// cambia por un salto y devuelve true; si no, no cambia nada.
    bool backslash_newline();
    /// Cursor en bytes.
    [[nodiscard]] std::size_t cursor() const;
    /// Filas que mide el texto en el campo (con el ancho del último cuadro).
    [[nodiscard]] int row_count() const;
    /// Alto máximo (0: el que le den); la caja lo cambia con la terminal.
    void set_max_rows(int rows);
    void set_on_enter(std::function<void()> on_enter);
    /// true si (x, y) cae en el campo (último cuadro).
    [[nodiscard]] bool contains(int x, int y) const;
    /// true si el texto no cabe y el campo tiene barra (último cuadro): la
    /// rueda sobre él lo desplaza.
    [[nodiscard]] bool scrollable() const;
    /// El modelo (para las pruebas).
    [[nodiscard]] const TextEdit& edit() const;

    /// Filas que mueve cada paso de la rueda (como el historial).
    static constexpr int kWheelStep = 3;

    /// Lo que el dibujo comparte con el componente: el modelo y lo que midió
    /// el último cuadro (ancho del texto, alto, caja, barra).
    struct State;

    ftxui::Element OnRender() override;
    bool OnEvent(ftxui::Event event) override;
    [[nodiscard]] bool Focusable() const override { return true; }

private:

    bool handle_mouse(ftxui::Event event);
    bool handle_key(const ftxui::Event& event);
    void changed();

    std::shared_ptr<State> state_;
    bool hovered_ = false;
};

/// Un TextArea nuevo.
[[nodiscard]] std::shared_ptr<TextArea> make_text_area(TextAreaOption option);

} // namespace chatbot::cli

#endif // CHATBOT_CLI_TEXT_AREA_H
