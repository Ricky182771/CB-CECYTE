#ifndef CHATBOT_CLI_PASTE_H
#define CHATBOT_CLI_PASTE_H

#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace chatbot::cli {

// Pegado de texto (sin FTXUI ni Win32): saneado, destino y el acumulador del
// pegado entre corchetes (bracketed paste, DECSET 2004). main.cpp traduce los
// eventos de FTXUI y lee el portapapeles; aquí solo está la lógica.

/// Tope de un pegado, en bytes UTF-8 ya saneados (y de lo que se acumula
/// entre las marcas del pegado entre corchetes).
inline constexpr std::size_t kMaxPasteBytes = 256 * 1024;

/// Activa y desactiva el pegado entre corchetes (DECSET/DECRST 2004).
inline constexpr std::string_view kBracketedPasteOn = "\x1B[?2004h";
inline constexpr std::string_view kBracketedPasteOff = "\x1B[?2004l";
/// Marcas que manda la terminal alrededor del texto pegado. FTXUI v7.0.3 no
/// conoce el modo 2004: su ParseCSI las entrega como Event::Special con esta
/// misma secuencia.
inline constexpr std::string_view kPasteStartMark = "\x1B[200~";
inline constexpr std::string_view kPasteEndMark = "\x1B[201~";
/// Shift+Insert en modo VT (conhost, Windows Terminal y xterm): ESC [ 2 ; 2 ~.
/// FTXUI v7.0.3 no lo uniformiza: llega como Event::Special con esta secuencia.
inline constexpr std::string_view kShiftInsert = "\x1B[2;2~";

/// Aviso de un pegado recortado. Nunca lleva el texto (puede ser una key).
inline constexpr std::string_view kPasteTruncated =
    "El texto pegado era muy largo: se pegaron los primeros 256 KiB.";
/// Aviso de un atajo de pegado sin texto en el portapapeles.
inline constexpr std::string_view kClipboardNoText = "El portapapeles no tiene texto.";

/// Texto pegado listo para insertar.
struct SanitizedPaste {
    std::string text;
    bool truncated = false; ///< Se recortó a kMaxPasteBytes.
};

/// Limpia el texto pegado:
/// - "\r\n" y "\r" → "\n"; "\t" → 4 espacios;
/// - fuera los demás controles C0, DEL (0x7F) y los C1 (U+0080 a U+009F);
/// - cada byte de UTF-8 inválido → U+FFFD;
/// - multiline == false: fuera los "\n" del principio y del final, y los de
///   en medio → un espacio (una key copiada de una página con salto final);
/// - recortado a kMaxPasteBytes sin partir un carácter (truncated = true).
[[nodiscard]] SanitizedPaste sanitize_paste(std::string_view text, bool multiline);

/// A dónde iría un pegado.
enum class PasteTarget {
    None,       ///< Se ignora: la barra lateral, una lista, un botón o una pregunta.
    SingleLine, ///< Un campo de una línea (URL, key, filtro, modelo).
    MultiLine,  ///< La caja de la conversación o las instrucciones del sistema.
};

/// El destino según el foco: con la barra lateral, None; con la
/// configuración abierta, el campo que tiene el foco (settings_field); si
/// no, la caja de la conversación.
[[nodiscard]] PasteTarget paste_target(bool sidebar_focused, bool settings_open,
                                       PasteTarget settings_field);

/// Aviso de un atajo de pegado sin destino (una lista, un botón o una pregunta).
inline constexpr std::string_view kPasteNoTarget = "Aquí no se puede pegar.";

/// Aviso de un atajo de pegado según el destino: kPasteNoTarget si es None,
/// salvo con la barra lateral, donde se ignora sin aviso; nullopt si hay
/// destino.
[[nodiscard]] std::optional<std::string_view> paste_shortcut_notice(bool sidebar_focused,
                                                                    PasteTarget target);

/// sanitize_paste según el destino. nullopt si se ignora (None) o si no
/// queda texto que insertar.
[[nodiscard]] std::optional<SanitizedPaste> prepare_paste(std::string_view raw,
                                                          PasteTarget target);

/// Un evento de la terminal, para BracketedPaste.
enum class PasteKey {
    Start,       ///< ESC [ 200 ~
    End,         ///< ESC [ 201 ~
    Character,   ///< Un carácter (Event::Character).
    Return,      ///< Event::Return: un salto de línea pegado.
    Tab,         ///< Event::Tab.
    Passthrough, ///< Ratón o Event::Custom: nunca es parte del pegado.
    Other,       ///< Cualquier otro evento especial.
};

/// Pausa que cierra un pegado sin marca de fin: si entre un evento del
/// pegado y el siguiente evento pasa más que esto, el pegado se da por
/// terminado (una terminal que nunca mandó 201~ no deja la app atorada).
inline constexpr std::chrono::milliseconds kPasteIdleLimit{500};

/// Acumula lo que llega entre las marcas del pegado entre corchetes. Se usa
/// solo desde el hilo de la interfaz.
class BracketedPaste {
public:
    /// El reloj (steady_clock::now, o uno falso en las pruebas).
    using Clock = std::function<std::chrono::steady_clock::time_point()>;

    explicit BracketedPaste(Clock clock = [] { return std::chrono::steady_clock::now(); })
        : clock_(std::move(clock)) {}

    struct Step {
        /// El evento era del pegado (o una marca): no sigue a la interfaz.
        bool consumed = false;
        /// El pegado terminó: el texto crudo (sin sanear).
        std::optional<std::string> text;
        /// Se dejó de acumular al pasar de kMaxPasteBytes.
        bool truncated = false;
    };

    /// Procesa un evento. character: el texto de un PasteKey::Character.
    /// - Salida de emergencia, sin temporizador: si hay un pegado y pasó más
    ///   de kPasteIdleLimit desde su último evento, se entrega lo acumulado
    ///   (como si hubiera llegado End) y este evento se procesa como si no
    ///   hubiera pegado: una tecla no se consume y sigue a la interfaz.
    /// - Start empieza a acumular; si ya había un pegado sin End, ese se
    ///   entrega completo y empieza otro.
    /// - End entrega lo acumulado; un End sin Start se descarta.
    /// - Mientras acumula: Character tal cual, Return como "\n", Tab como
    ///   "\t"; Other se descarta. Todos se consumen.
    /// - Passthrough nunca se consume; solo puede cerrar un pegado por la
    ///   pausa.
    Step feed(PasteKey key, std::string_view character = {});

    /// true entre Start y End.
    [[nodiscard]] bool active() const { return active_; }

private:
    /// feed sin la salida de emergencia.
    Step process(PasteKey key, std::string_view character);
    void append(std::string_view piece);
    Step finish();

    Clock clock_;
    std::chrono::steady_clock::time_point last_{}; ///< Último evento del pegado.
    bool active_ = false;
    bool truncated_ = false;
    std::string buffer_;
};

} // namespace chatbot::cli

#endif // CHATBOT_CLI_PASTE_H
