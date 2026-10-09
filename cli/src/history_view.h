#ifndef CHATBOT_CLI_HISTORY_VIEW_H
#define CHATBOT_CLI_HISTORY_VIEW_H

#include "conversation.h"
#include "markdown.h"
#include "theme.h"

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace chatbot::cli {

/// Dibuja las entradas de la conversación. Las respuestas del asistente se
/// muestran como markdown (también mientras llegan); las del usuario, los
/// errores y los avisos, como texto plano filtrado.
///
/// Caché por entrada, en dos niveles:
/// - el árbol de markdown y el texto del que salió: solo se vuelve a parsear
///   si el texto cambió;
/// - la entrada ya dibujada para un ancho y una paleta: se vuelve a dibujar
///   si cambió la entrada, el ancho, el tema, el modo de fondo o el número
///   de su primer bloque de código. En cada cuadro solo se copian las filas
///   visibles.
///
/// Cada entrada se dibuja con el fondo y el color de texto de la paleta
/// (Palette::base), porque se copia celda por celda sobre la pantalla.
///
/// Se usa solo desde el hilo de la interfaz.
class HistoryView {
public:
    /// first_code_numbers[i]: número del primer bloque de código de la
    /// entrada i (CodeBlockIndex::first_numbers o first_code_numbers, en
    /// code_blocks.h); los bloques se dibujan como "#3 · cpp". Si falta (por
    /// ejemplo, con el vector vacío), los bloques de esa entrada van sin número.
    [[nodiscard]] ftxui::Element render(const std::vector<Entry>& entries, int width,
                                        const Palette& palette,
                                        const std::vector<int>& first_code_numbers = {});

    /// Veces que se parseó markdown (para las pruebas de la caché).
    [[nodiscard]] std::size_t parse_count() const { return parse_count_; }
    /// Veces que se dibujó una entrada (para las pruebas de la caché).
    [[nodiscard]] std::size_t draw_count() const { return draw_count_; }

private:
    struct Cached {
        // Árbol de markdown (solo entradas del asistente).
        bool parsed = false;
        std::string source;
        md::Document document;
        // Entrada dibujada y lo que se usó para dibujarla.
        std::shared_ptr<const ftxui::Screen> image;
        int width = 0;
        std::string palette_key; ///< Palette::key() con que se dibujó.
        int first_code = 0;      ///< Número del primer bloque de código (0: sin números).
        Entry drawn;
    };

    const md::Document& document_for(Cached& cached, const std::string& text);
    ftxui::Element entry_element(Cached& cached, const Entry& entry, int width,
                                 const Palette& palette, int first_code);

    std::vector<Cached> cache_;
    std::size_t parse_count_ = 0;
    std::size_t draw_count_ = 0;
};

} // namespace chatbot::cli

#endif // CHATBOT_CLI_HISTORY_VIEW_H
