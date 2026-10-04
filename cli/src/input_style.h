#ifndef CHATBOT_CLI_INPUT_STYLE_H
#define CHATBOT_CLI_INPUT_STYLE_H

#include <ftxui/dom/elements.hpp>

namespace chatbot::cli {

/// Estilo de la caja de entrada, para InputOption::transform (main.cpp lo
/// adapta desde ftxui::InputState: aquí van sus campos sueltos para no
/// depender de ftxui::component). A diferencia del transform por defecto de
/// FTXUI v7.0.3 (InputOption::Default), no invierte la línea con el foco ni
/// la subraya con el ratón encima: el fondo invertido toma el color del texto
/// del tema y choca con temas translúcidos. El cursor no depende de esto: lo
/// pone el propio Input con focusCursorBarBlinking/BlockBlinking. Solo el
/// placeholder va en dim. hovered y focused no cambian nada a propósito.
[[nodiscard]] ftxui::Element input_transform(ftxui::Element element, bool hovered, bool focused,
                                             bool is_placeholder);

} // namespace chatbot::cli

#endif // CHATBOT_CLI_INPUT_STYLE_H
