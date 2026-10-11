#ifndef CHATBOT_CLI_COMMAND_MENU_VIEW_H
#define CHATBOT_CLI_COMMAND_MENU_VIEW_H

#include "command_menu.h"
#include "commands.h"
#include "theme.h"

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>

#include <vector>

namespace chatbot::cli {

/// Columnas mínimas de la descripción: con menos, se oculta y el nombre usa
/// todo el ancho.
inline constexpr int kMenuMinDescription = 12;

/// Dibuja el menú de comandos abierto, de width columnas: una línea en el
/// color de borde y una fila por comando que coincide, con "nombre
/// argumentos" alineados al ancho del más largo del registro y la
/// descripción (o, si no está disponible, la nota) a la derecha, recortada
/// con "…". En pantallas angostas la descripción se oculta antes que el
/// nombre. El cursor lleva Palette::selection() a todo lo ancho; un comando
/// no disponible va en el color notice. Sin coincidencias, la fila
/// CommandMenu::kNoMatches, en notice y sin cursor. availability es la de
/// command_availability (un elemento por comando de kCommands). row_boxes
/// recibe la caja de cada fila de comando (una por menu.matches()).
[[nodiscard]] ftxui::Element render_command_menu(
    const CommandMenu& menu, const std::vector<CommandAvailability>& availability, int width,
    const Palette& palette, std::vector<ftxui::Box>* row_boxes = nullptr);

} // namespace chatbot::cli

#endif // CHATBOT_CLI_COMMAND_MENU_VIEW_H
