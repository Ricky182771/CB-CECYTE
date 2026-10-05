#ifndef CHATBOT_CLI_SIDEBAR_VIEW_H
#define CHATBOT_CLI_SIDEBAR_VIEW_H

#include "sidebar.h"
#include "theme.h"

#include <ftxui/dom/elements.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace chatbot::cli {

/// Ancho de la barra lateral con sus dos bordes: por defecto, mínimo y máximo
/// (el máximo también se limita a la mitad de la terminal en main.cpp).
inline constexpr int kSidebarWidth = 28;
inline constexpr int kSidebarMinWidth = 18;
inline constexpr int kSidebarMaxWidth = 60;
/// Ancho de terminal desde el que la barra se muestra al arrancar.
inline constexpr int kSidebarMinTerminal = 100;

/// Recorta text a width columnas (medidas con ftxui::string_width) y pone
/// "…" al final si no cabía. Nunca parte un carácter, aunque sea ancho.
[[nodiscard]] std::string fit_width(std::string_view text, int width);

/// Filas de la lista que caben en una barra de height líneas (sin los bordes
/// de arriba y abajo); al menos 1.
[[nodiscard]] int sidebar_view_height(int height);

/// Dibuja la barra lateral, sin su borde derecho: ese borde es
/// sidebar_divider(), el divisor que se arrastra con el ratón. width son las
/// columnas sin ese borde y height el alto total. Muestra las filas visibles
/// desde sidebar.top(); la fila del cursor lleva la selección solo si
/// focused, y la conversación abierta, "● " (list_style.h).
[[nodiscard]] ftxui::Element render_sidebar(const Sidebar& sidebar,
                                            const std::vector<SidebarRow>& rows, int width,
                                            int height, bool focused, const Palette& palette);

/// Borde derecho de la barra ("┐", "│"…, "┘"), que también es el divisor de
/// ResizableSplit.
[[nodiscard]] ftxui::Element sidebar_divider(const Palette& palette);

} // namespace chatbot::cli

#endif // CHATBOT_CLI_SIDEBAR_VIEW_H
