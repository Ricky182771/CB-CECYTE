#ifndef CHATBOT_BOT_SPLIT_H
#define CHATBOT_BOT_SPLIT_H

#include "chatbot/bot/types.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace chatbot::bot {

/// Tope mínimo que aceptan las funciones de partir: uno menor se trata como
/// este (siempre debe caber cualquier carácter y una cerca corta).
inline constexpr std::size_t kMinMessageLimit = 16;

/// Parte la respuesta del modelo (markdown) en pedazos que caben en
/// limit.max_size según limit.measure. Corta entre bloques de primer nivel
/// (md::source_blocks), juntando los que quepan. Un bloque que no cabe solo:
/// - código con cerca: se parte por líneas en varios bloques, cada uno con
///   la línea de la cerca original (y su lenguaje) y una cerca que cierra;
/// - lo demás (párrafos, listas, tablas…): por líneas, luego en un espacio
///   y, si no hay, entre caracteres.
/// Ningún pedazo pasa del tope ni queda vacío (ni solo con espacios); a cada
/// uno se le quitan las líneas en blanco del principio y los espacios del
/// final. Juntos tienen el mismo contenido que el original, salvo los
/// espacios donde se cortó y las cercas que se repiten.
[[nodiscard]] std::vector<std::string> split_markdown(std::string_view markdown,
                                                      const MessageLimit& limit);

/// Igual, para texto sin formato (avisos, fuentes): por líneas, luego en un
/// espacio y, si no hay, entre caracteres.
[[nodiscard]] std::vector<std::string> split_plain(std::string_view text,
                                                   const MessageLimit& limit);

/// true si text, sin las líneas en blanco del principio ni los espacios del
/// final (como sale cada pedazo), cabe en un mensaje: el mismo criterio que
/// split_markdown y split_plain.
[[nodiscard]] bool fits_in_message(std::string_view text, const MessageLimit& limit);

/// Una respuesta que llega por partes y se muestra en una vista previa
/// (Draft). Mientras lo pendiente quepa en un mensaje, la vista previa lo
/// muestra todo; cuando ya no cabe, take_complete() corta lo que sí cabe
/// para mandarlo como mensaje final y la vista previa sigue con el resto.
/// Corta entre bloques de primer nivel si puede; si no, entre líneas, en un
/// espacio o entre caracteres (como split_markdown). Si el corte cae dentro
/// de un bloque de código con cerca, el resto empieza con la línea de esa
/// cerca, para que siga siendo código. No es segura entre hilos.
class StreamingReply {
public:
    explicit StreamingReply(MessageLimit limit) : limit_(std::move(limit)) {}

    void append(std::string_view delta) { text_ += delta; }

    /// Los mensajes que ya se pueden mandar (en orden), o vacío si lo
    /// pendiente todavía cabe. Lo que devuelve deja de estar pendiente.
    [[nodiscard]] std::vector<std::string> take_complete();

    /// Lo que falta mandar: la cerca que se repite (si el último corte cayó
    /// dentro de un bloque de código) y lo que llegó después del corte.
    [[nodiscard]] std::string pending() const;

    /// true si ya se mandó una parte con take_complete().
    [[nodiscard]] bool has_taken() const { return taken_; }

    /// La respuesta completa, como llegó (para el historial).
    [[nodiscard]] const std::string& text() const { return text_; }

private:
    MessageLimit limit_;
    std::string text_;
    std::size_t offset_ = 0; ///< Lo que ya se mandó de text_.
    std::string carry_;      ///< Línea de la cerca que se repite.
    bool taken_ = false;
};

} // namespace chatbot::bot

#endif // CHATBOT_BOT_SPLIT_H
