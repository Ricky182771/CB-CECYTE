#ifndef CHATBOT_CLI_MARKDOWN_VIEW_H
#define CHATBOT_CLI_MARKDOWN_VIEW_H

#include "markdown.h"
#include "theme.h"

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace chatbot::cli::md {

/// Dibuja el documento para un ancho de width columnas. El ajuste de líneas
/// se calcula aquí (no en FTXUI), así que el alto del resultado depende solo
/// del documento y del ancho. Nunca pierde texto: lo que no cabe se parte.
/// Los colores salen de palette; el fondo y el color del texto normal no:
/// los pone quien llama (Palette::base) sobre todo lo que dibuja.
/// first_code > 0 numera los bloques de código desde ahí, en el orden de
/// code_blocks_of (code_blocks.h): el título del marco pasa de "cpp" a
/// "#3 · cpp", o "#3" sin lenguaje. Con 0, sin números.
/// code_boxes: si no es nulo, queda con un elemento por bloque de código, en
/// el orden de code_blocks_of, con la caja de su marco (la que recibe al
/// dibujar el elemento: en Render(screen, element), en coordenadas de esa
/// pantalla). Con nullptr la salida es idéntica.
[[nodiscard]] ftxui::Element render(const Document& document, int width,
                                    const Palette& palette, int first_code = 0,
                                    std::vector<ftxui::Box>* code_boxes = nullptr);

/// Título del marco de un bloque de código, con un espacio a cada lado:
/// " #3 · cpp ", " #3 " (sin lenguaje), " cpp " (sin número: number = 0) o
/// vacío si no hay ninguno de los dos.
[[nodiscard]] std::string code_title(int number, std::string_view info);

/// Texto plano (sin markdown) filtrado con sanitize() y ajustado a width
/// columnas con las mismas reglas; conserva los espacios repetidos. style
/// (por ejemplo, Palette::ink) se aplica solo al texto de cada línea, no al
/// relleno hasta el borde.
[[nodiscard]] ftxui::Element render_plain(std::string_view text, int width,
                                          const ftxui::Decorator& style = {});

/// Anchos (columnas de pantalla, sin bordes) que render() da a las columnas
/// de un bloque Table con ese ancho total; vacío si la tabla se dibuja como
/// tarjetas. Sirve para probar y medir el reparto.
[[nodiscard]] std::vector<int> table_column_widths(const Block& table, int width);

/// Prepara una URL para ftxui::hyperlink, que la escribe tal cual dentro de
/// una secuencia OSC 8: todo byte fuera de 0x21-0x7E se codifica como %XX.
[[nodiscard]] std::string hyperlink_target(std::string_view url);

} // namespace chatbot::cli::md

#endif // CHATBOT_CLI_MARKDOWN_VIEW_H
