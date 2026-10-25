#ifndef CHATBOT_CLI_CHILD_PROCESS_H
#define CHATBOT_CLI_CHILD_PROCESS_H

#include "chatbot/cancel_token.h"

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/// Programas externos: el portapapeles (wl-copy, xclip…), systemctl,
/// loginctl, journalctl y chatbot-telegram --config-version. Sin shell:
/// posix_spawnp con los argumentos tal cual. CMake compila
/// child_process_posix.cpp o child_process_windows.cpp (ahí solo lo mínimo
/// para compilar: no se lanzan programas). Nunca desde el hilo de la
/// interfaz si puede tardar. No se llama process.h: con cli/src en el
/// include path, la pthread.h de MinGW tomaría este en lugar del <process.h>
/// del sistema.
namespace chatbot::cli {

/// Tope de lo que se guarda de stdout y de stderr; el resto se lee y se tira
/// (el programa no se queda esperando a que alguien lea).
inline constexpr std::size_t kMaxProcessOutput = 1024 * 1024;

struct ProcessOptions {
    /// Lo que recibe por stdin (vacío: /dev/null).
    std::string_view input;
    /// Tope total; después se mata con SIGKILL.
    std::chrono::milliseconds timeout{5000};
    /// true: stdout y stderr se capturan; false: van a /dev/null (para un
    /// programa que deja un hijo con la salida abierta, como wl-copy).
    bool capture_output = true;
    /// Al cancelarse: SIGTERM al hijo, unos 200 ms para que salga y después
    /// SIGKILL. Quien lo pasa lo mantiene vivo mientras corre.
    const CancelToken* cancel = nullptr;
};

struct ProcessResult {
    enum class Status {
        Exited,   ///< Terminó (exit_code).
        Signaled, ///< Lo mató una señal que no fue el tope.
        TimedOut, ///< Se pasó del tope y se mató.
        Cancelled, ///< Se canceló (ProcessOptions::cancel) y se mató.
        NotFound, ///< No existe el programa.
        Failed,   ///< No se pudo lanzar (o en Windows).
    };
    Status status = Status::Failed;
    int exit_code = -1;          ///< Solo con Exited.
    bool input_written = false;  ///< Recibió todo ProcessOptions::input.
    std::string out;             ///< stdout, hasta kMaxProcessOutput.
    std::string err;             ///< stderr, hasta kMaxProcessOutput.

    /// Salió con 0.
    [[nodiscard]] bool ok() const { return status == Status::Exited && exit_code == 0; }
};

/// Lanza argv[0] (buscado en PATH si no trae '/') con argv, sin shell; el
/// hijo empieza sin señales bloqueadas. Lee su salida mientras corre y
/// regresa en cuanto termina (aunque un nieto se quede con la salida
/// abierta), al pasar el tope o al cancelarse.
[[nodiscard]] ProcessResult run_process(const std::vector<std::string>& argv,
                                        const ProcessOptions& options = {});

/// Un archivo normal (no carpeta) que se puede ejecutar. En Windows, false.
[[nodiscard]] bool is_executable_file(const std::string& path);

/// Reemplaza este proceso por path con argv (execv, sin shell ni PATH). Solo
/// regresa si falla, con el motivo. En Windows, siempre falla.
[[nodiscard]] std::string exec_program(const std::string& path,
                                       const std::vector<std::string>& argv);

/// El UID numérico de este proceso (getuid). En Windows, nullopt.
[[nodiscard]] std::optional<unsigned long> current_user_id();

/// La ruta del primer archivo ejecutable con ese nombre en las carpetas de
/// PATH (sin la carpeta actual), o nullopt. Un nombre con '/' no se busca.
/// En Windows, siempre nullopt: ahí no se lanzan programas.
[[nodiscard]] std::optional<std::string> find_in_path(std::string_view program);

} // namespace chatbot::cli

#endif // CHATBOT_CLI_CHILD_PROCESS_H
