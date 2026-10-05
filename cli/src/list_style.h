#ifndef CHATBOT_CLI_LIST_STYLE_H
#define CHATBOT_CLI_LIST_STYLE_H

#include "theme.h"

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace chatbot::cli {

// Selección y foco, iguales en toda la interfaz:
// - Cursor: la fila completa con Palette::selection(), sin prefijo "> ".
//   Solo mientras la lista tiene el foco.
// - Elegido: "● " en heading_accent y negritas; las demás filas llevan dos
//   espacios. Se ve tenga o no el foco la lista.
// - Lo demás, con el color de texto normal (sin dim).

/// Marca del elegido y lo que llevan las demás filas en su lugar.
inline constexpr std::string_view kChosenMark = "● ";
inline constexpr std::string_view kNotChosenMark = "  ";

/// Marca de la fila: "● " en heading_accent y negritas si chosen; si no, dos
/// espacios.
[[nodiscard]] ftxui::Element chosen_mark(bool chosen, const Palette& palette);

/// Fila de una lista: la marca y label (ya sin controles de terminal). Con
/// cursor, toda la fila con la selección.
[[nodiscard]] ftxui::Element list_row(std::string_view label, bool cursor, bool chosen,
                                      const Palette& palette);

/// Lista con scroll vertical: la fila cursor siempre queda visible (focus
/// dentro del frame) y lleva la selección solo si focused. chosen: la fila
/// elegida, si hay.
[[nodiscard]] ftxui::Element choice_list(const std::vector<std::string>& labels,
                                         std::size_t cursor, std::optional<std::size_t> chosen,
                                         bool focused, const Palette& palette,
                                         std::vector<ftxui::Box>* row_boxes = nullptr);

/// Etiqueta de un campo de formulario: con el foco, con la selección.
[[nodiscard]] ftxui::Element field_label(std::string_view text, bool focused,
                                         const Palette& palette);

/// Botón "[ Guardar ]": con el foco, con la selección; sin él, texto normal.
[[nodiscard]] ftxui::Element button_label(std::string_view label, bool focused,
                                          const Palette& palette);

} // namespace chatbot::cli

#endif // CHATBOT_CLI_LIST_STYLE_H
