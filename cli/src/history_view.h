#ifndef CHATBOT_CLI_HISTORY_VIEW_H
#define CHATBOT_CLI_HISTORY_VIEW_H

#include "block_actions.h"
#include "conversation.h"
#include "markdown.h"
#include "theme.h"

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>
#include <ftxui/screen/screen.hpp>

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace chatbot::cli {

/// Un bloque de código ya dibujado en una entrada.
struct CodeFrame {
    int number = 0;      ///< Número del bloque (el de collect_code_blocks).
    ftxui::Box box;      ///< Marco del bloque, en coordenadas de la entrada.
    int title_width = 0; ///< Columnas del título del marco (" #3 · cpp ").
};

/// Un botón dibujado en el último cuadro.
struct ButtonHit {
    ftxui::Box box;  ///< En coordenadas de la pantalla (una fila).
    int block = 0;   ///< Número del bloque (el de collect_code_blocks).
    BlockAction action = BlockAction::Copy;
};

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
/// Botones [Copiar] [Guardar] de los bloques de código numerados: se dibujan
/// en cada cuadro sobre las celdas ya copiadas (no son parte de la caché),
/// alineados a la derecha y terminando una columna antes del borde derecho
/// del marco. Van sobre el borde superior si está a la vista; si no, fijos
/// en la primera fila visible del contenido, nunca sobre el borde inferior
/// ni fuera del bloque. Si no caben después del título con 2 columnas de
/// margen, [C] [G]; si tampoco, no hay botones. Tras copiar, [✓ Copiado] (o
/// [✓]) hasta que el puntero sale del botón o pasan kCopiedFor, revisado en
/// el siguiente cuadro (sin hilos ni temporizadores). Ni el puntero encima
/// (hover) ni ese aviso vuelven a dibujar ninguna entrada.
///
/// Se usa solo desde el hilo de la interfaz.
class HistoryView {
public:
    /// Reloj para el aviso de copiado (uno falso en las pruebas).
    using Clock = std::function<std::chrono::steady_clock::time_point()>;
    /// Cuánto dura [✓ Copiado] si el puntero no sale del botón.
    static constexpr std::chrono::seconds kCopiedFor{2};

    explicit HistoryView(Clock clock = std::chrono::steady_clock::now)
        : clock_(std::move(clock)) {}

    /// first_code_numbers[i]: número del primer bloque de código de la
    /// entrada i (CodeBlockIndex::first_numbers o first_code_numbers, en
    /// code_blocks.h); los bloques se dibujan como "#3 · cpp". Si falta (por
    /// ejemplo, con el vector vacío), los bloques de esa entrada van sin número.
    [[nodiscard]] ftxui::Element render(const std::vector<Entry>& entries, int width,
                                        const Palette& palette,
                                        const std::vector<int>& first_code_numbers = {});

    /// El botón que se dibujó en (x, y) de la pantalla, o nullopt. Es del
    /// cuadro anterior (el último que se dibujó): un clic llega después de
    /// verlo, así que es lo que el usuario tenía en pantalla.
    [[nodiscard]] std::optional<ButtonHit> hit_test(int x, int y) const;

    /// Puntero en (x, y) de la pantalla: el botón que esté ahí se dibuja con
    /// el color de selección desde el siguiente cuadro.
    void set_hover(int x, int y);
    /// El puntero salió del historial: ningún botón con hover (y se quita
    /// [✓ Copiado]).
    void clear_hover();
    /// Se copió el bloque: su botón dice [✓ Copiado] hasta que el puntero
    /// sale de él (set_hover, clear_hover) o pasan kCopiedFor (en render()).
    void show_copied(int block);
    /// true (por defecto) dibuja los botones de los bloques.
    void set_buttons_visible(bool visible) { buttons_visible_ = visible; }

    /// Bloques de código de la entrada i en el último render (vacío si no
    /// tiene o si se dibujó sin números).
    [[nodiscard]] const std::vector<CodeFrame>& code_frames(std::size_t entry) const;

    /// Veces que se parseó markdown (para las pruebas de la caché).
    [[nodiscard]] std::size_t parse_count() const { return parse_count_; }
    /// Veces que se dibujó una entrada (para las pruebas de la caché).
    [[nodiscard]] std::size_t draw_count() const { return draw_count_; }

private:
    class Picture;

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
        /// Marcos de los bloques de código (md::render los llena al dibujar).
        std::vector<ftxui::Box> code_boxes;
        /// Lo mismo con número y título, ya en coordenadas de la entrada.
        std::vector<CodeFrame> code_frames;
    };

    const md::Document& document_for(Cached& cached, const std::string& text);
    ftxui::Element entry_element(Cached& cached, const Entry& entry, int width,
                                 const Palette& palette, int first_code);

    std::vector<Cached> cache_;
    Palette palette_; ///< La del último render, para los botones.
    std::optional<std::pair<int, int>> hover_; ///< Puntero (x, y), si está encima.
    /// Botones del último cuadro: render() lo vacía y Picture::Render lo llena.
    std::vector<ButtonHit> hits_;
    bool buttons_visible_ = true;
    Clock clock_;
    int copied_block_ = 0; ///< Bloque con [✓ Copiado], o 0.
    std::chrono::steady_clock::time_point copied_at_;
    std::size_t parse_count_ = 0;
    std::size_t draw_count_ = 0;
};

} // namespace chatbot::cli

#endif // CHATBOT_CLI_HISTORY_VIEW_H
