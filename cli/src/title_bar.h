#ifndef CHATBOT_CLI_TITLE_BAR_H
#define CHATBOT_CLI_TITLE_BAR_H

#include "theme.h"

#include <ftxui/screen/box.hpp>

#include <string_view>

namespace chatbot::cli {

/// Botón [Exportar] de la barra de título.
struct ExportButton {
    /// Hay respuestas que exportar (color del texto); si no, atenuado
    /// (input_placeholder) y sin acción.
    bool enabled = false;
    bool hovered = false; ///< El puntero está encima (con enabled, en el color de selección).
};

/// Etiqueta del botón de exportar.
inline constexpr std::string_view kExportLabel = "[Exportar]";

/// Título a la izquierda; [Exportar] y la ayuda de teclas a la derecha. El
/// título ocupa primero el ancho, luego el botón y al final la ayuda: la
/// ayuda se acorta o desaparece antes que el botón, y el botón desaparece
/// antes de recortar el título con puntos suspensivos. box queda con la
/// caja del botón al dibujar la barra (con reflect), o vacía si no cupo.
[[nodiscard]] ftxui::Element title_bar(std::string_view title, int width,
                                      const Palette& palette, const ExportButton& button,
                                      ftxui::Box& box);

} // namespace chatbot::cli

#endif
