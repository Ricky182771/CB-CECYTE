#ifndef CHATBOT_CLI_STATUS_LINE_H
#define CHATBOT_CLI_STATUS_LINE_H

#include <ftxui/dom/elements.hpp>

#include <string>
#include <vector>

namespace chatbot::cli {

/// Una parte de la línea de estado.
struct StatusItem {
    std::string text;
    ftxui::Decorator style = ftxui::nothing; ///< Vacío ({}): sin estilo.
    /// Indicador ("Pensando…", "↓ Hay más abajo"): tiene su ancho reservado
    /// y nunca se recorta.
    bool reserved = false;
};

/// La línea de estado en una fila de width columnas, con las partes en su
/// orden y separadas por tres espacios. Primero se reserva el ancho de los
/// indicadores (si ni ellos caben, se quitan desde el último, nunca el
/// primero); las demás partes (avisos) usan lo que sobra, en orden, y la que
/// no cabe termina en "…" (las siguientes no se muestran; con menos de 2
/// columnas libres, tampoco ella). Sin partes, una fila vacía: la línea mide
/// siempre una fila.
[[nodiscard]] ftxui::Element status_line(const std::vector<StatusItem>& items, int width);

} // namespace chatbot::cli

#endif // CHATBOT_CLI_STATUS_LINE_H
