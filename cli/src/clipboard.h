#ifndef CHATBOT_CLI_CLIPBOARD_H
#define CHATBOT_CLI_CLIPBOARD_H

#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace chatbot::cli {

/// Lee una variable de entorno: nullopt si no está definida.
using EnvLookup = std::function<std::optional<std::string>(std::string_view name)>;
/// true si el programa existe en PATH (find_in_path, o uno falso en las pruebas).
using ProgramFinder = std::function<bool(std::string_view program)>;
/// Lanza argv (sin shell) con input en su stdin; true si salió con código 0.
using ProgramRunner =
    std::function<bool(const std::vector<std::string>& argv, std::string_view input)>;
/// Escribe una secuencia en la terminal; false si no se pudo.
using TerminalWriter = std::function<bool(std::string_view sequence)>;

/// Tope de texto (antes de codificar) que se manda por OSC 52.
inline constexpr std::size_t kOsc52MaxBytes = 100000;
/// Tope de espera por un programa de portapapeles.
inline constexpr std::chrono::milliseconds kClipboardTimeout{2000};

/// Una forma de copiar al portapapeles.
struct ClipboardMethod {
    enum class Kind {
        Program, ///< Un programa externo que lee el texto de stdin.
        Osc52,   ///< La secuencia OSC 52, escrita en la terminal.
    };
    Kind kind = Kind::Program;
    /// Program: el programa y sus argumentos fijos. (Windows, clip.exe: sería
    /// otro Program; queda para el port.)
    std::vector<std::string> argv;
    /// Osc52: true si es el último recurso (no hubo programa que funcionara).
    bool fallback = false;

    /// Nombre para el aviso: el programa ("wl-copy") o "OSC 52".
    [[nodiscard]] std::string name() const;
};

/// Orden de intento (función pura, sin lanzar nada):
/// 1. con SSH_CONNECTION o SSH_TTY, OSC 52 primero (llega a la máquina local);
/// 2. termux-clipboard-set (TERMUX_VERSION), wl-copy (WAYLAND_DISPLAY) y
///    `xclip -selection clipboard` o `xsel --clipboard --input` (DISPLAY),
///    cada uno solo si find lo encuentra;
/// 3. OSC 52 como último recurso (fallback = true).
/// Una variable vacía cuenta como no definida.
[[nodiscard]] std::vector<ClipboardMethod> clipboard_methods(const EnvLookup& env,
                                                             const ProgramFinder& find);

/// Base64 estándar (RFC 4648, sección 4) con relleno "=".
[[nodiscard]] std::string base64_encode(std::string_view data);

/// ESC ] 52 ; c ; <base64> BEL. Con tmux, envuelta en ESC P tmux; … ESC \ con
/// los ESC de adentro duplicados.
[[nodiscard]] std::string osc52_sequence(std::string_view text, bool tmux);

/// Resultado de copiar.
struct CopyResult {
    bool copied = false;
    std::string method;    ///< Nombre del método que funcionó (ClipboardMethod::name).
    bool fallback = false; ///< Se copió con OSC 52 como último recurso.
    bool too_long = false; ///< OSC 52 no se intentó: el texto pasa de kOsc52MaxBytes.
};

/// Prueba los métodos en orden hasta que uno funcione. Un programa que
/// falla pasa al siguiente; OSC 52 se salta si el texto pasa de
/// kOsc52MaxBytes. tmux: si TMUX está definida.
[[nodiscard]] CopyResult copy_to_clipboard(std::string_view text,
                                           const std::vector<ClipboardMethod>& methods,
                                           bool tmux, const ProgramRunner& run,
                                           const TerminalWriter& write);

// Implementaciones reales (no se usan en las pruebas).

/// El entorno del proceso.
[[nodiscard]] std::optional<std::string> environment_value(std::string_view name);

/// Busca un archivo ejecutable con ese nombre en cada carpeta de PATH.
[[nodiscard]] bool find_in_path(std::string_view program);

/// Lanza argv con posix_spawnp (nunca un shell), con input por un pipe a su
/// stdin y stdout y stderr a /dev/null. Espera a lo más timeout; si se
/// pasa, lo mata. true si salió con código 0 y recibió todo el texto.
[[nodiscard]] bool run_with_input(const std::vector<std::string>& argv, std::string_view input,
                                  std::chrono::milliseconds timeout = kClipboardTimeout);

/// Escribe en std::cout (la terminal de FTXUI) y hace flush.
[[nodiscard]] bool write_to_terminal(std::string_view sequence);

} // namespace chatbot::cli

#endif // CHATBOT_CLI_CLIPBOARD_H
