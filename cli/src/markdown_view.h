#ifndef CHATBOT_CLI_MARKDOWN_VIEW_H
#define CHATBOT_CLI_MARKDOWN_VIEW_H

#include "markdown.h"

#include <ftxui/dom/elements.hpp>

#include <string>
#include <string_view>

namespace chatbot::cli::md {

/// Dibuja el documento para un ancho de width columnas. El ajuste de líneas
/// se calcula aquí (no en FTXUI), así que el alto del resultado depende solo
/// del documento y del ancho. Nunca pierde texto: lo que no cabe se parte.
[[nodiscard]] ftxui::Element render(const Document& document, int width);

/// Texto plano (sin markdown) filtrado con sanitize() y ajustado a width
/// columnas con las mismas reglas; conserva los espacios repetidos.
[[nodiscard]] ftxui::Element render_plain(std::string_view text, int width);

/// Prepara una URL para ftxui::hyperlink, que la escribe tal cual dentro de
/// una secuencia OSC 8: todo byte fuera de 0x21-0x7E se codifica como %XX.
[[nodiscard]] std::string hyperlink_target(std::string_view url);

} // namespace chatbot::cli::md

#endif // CHATBOT_CLI_MARKDOWN_VIEW_H
