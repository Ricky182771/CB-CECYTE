#ifndef CHATBOT_CLI_TITLE_BAR_H
#define CHATBOT_CLI_TITLE_BAR_H

#include "theme.h"

#include <string_view>

namespace chatbot::cli {

/// El título ocupa primero el ancho; la ayuda se acorta o desaparece antes
/// de recortarlo con puntos suspensivos.
[[nodiscard]] ftxui::Element title_bar(std::string_view title, int width,
                                      const Palette& palette);

} // namespace chatbot::cli

#endif
