#ifndef CHATBOT_CLI_SERVICE_CONTROL_H
#define CHATBOT_CLI_SERVICE_CONTROL_H

#include "child_process.h"

#include "chatbot/addon_settings/registry.h"
#include "chatbot/addon_settings/systemd_unit.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

/// El servicio de systemd de usuario de un addon, sin FTXUI: escribir la
/// unidad, systemctl --user, loginctl y journalctl (run_process, sin shell,
/// con tope de 5 s). Todo bloquea: nunca desde el hilo de la interfaz, sino
/// con ServiceWorker, que pasa su CancelToken a cada trabajo: al cancelarse,
/// el programa en curso se mata y no se corren los que faltan.
namespace chatbot::cli {

inline constexpr std::string_view kNoUserSystemd = "No hay systemd de usuario en esta sesión.";
inline constexpr std::string_view kNoLinger =
    "Sin linger, el bot se detiene al cerrar sesión y no arranca tras un apagón hasta que "
    "inicies sesión.";
inline constexpr std::string_view kLingerCommand = "loginctl enable-linger \"$USER\"";
inline constexpr std::string_view kReplaceForeignUnit = "¿Reemplazar el servicio existente? (s/n)";
/// Tope de cada programa.
inline constexpr std::chrono::milliseconds kServiceCommandTimeout{5000};

/// Corre un programa: run_process en la app; en las pruebas, uno falso.
using ProcessRunner =
    std::function<ProcessResult(const std::vector<std::string>&, const ProcessOptions&)>;

/// Cómo está el servicio.
struct ServiceStatus {
    bool systemd = true; ///< false: no hay systemctl o bus de usuario (lo demás, vacío).
    std::string active;  ///< is-active: "active", "inactive", "failed", "activating"…
    std::string enabled; ///< is-enabled: "enabled", "disabled", "not-found"…
    std::optional<int> exit_status; ///< ExecMainStatus, si no es 0.
    addon_settings::UnitState unit = addon_settings::UnitState::Missing;
    std::optional<bool> linger;     ///< nullopt: no se supo.
    std::string last_error;         ///< Última línea de error del registro, sin detalle.
};

/// El resultado de una acción, para la línea de estado.
struct ServiceAction {
    enum class Kind {
        Done,
        Confirm,  ///< Hay una unidad ajena: preguntar kReplaceForeignUnit.
        Failed,
        NoSystemd,
    };
    Kind kind = Kind::Done;
    std::string message;
};

/// Lo que dice F2 de un código de salida del addon (settings.h del bot), o
/// "" si no tiene uno propio: 65 (invitados.json dañado), 69 (no está
/// executable), 73 (la carpeta de datos data_dir) y 78 (configuración).
[[nodiscard]] std::string exit_status_message(int status, std::string_view executable,
                                              std::string_view data_dir);

/// El valor de property en la salida de "systemctl show -p A,B" (líneas
/// "A=valor"), o nullopt.
[[nodiscard]] std::optional<std::string> show_property(std::string_view show,
                                                       std::string_view property);

/// La última línea de error del registro (journalctl -o cat): las del bot
/// con nivel "error" y las fatales ("chatbot-telegram: …"). Sin la fecha y
/// sin lo que va desde " detalle=", que puede traer texto de los alumnos.
/// "" si no hay.
[[nodiscard]] std::string last_error_line(std::string_view journal,
                                          std::string_view executable);

/// El servicio corre según is-active: todo menos "inactive", "failed" y
/// nada (también "activating", el reinicio tras una falla).
[[nodiscard]] bool service_running(std::string_view active);

/// systemctl --user no pudo hablar con el systemd de usuario (sin
/// systemctl, o sin bus: por SSH sin sesión).
[[nodiscard]] bool no_user_systemd(const ProcessResult& result);

class ServiceControl {
public:
    /// unit_path: dónde va la unidad (default_unit_path). uid: el de
    /// loginctl (nullopt: no se pregunta por linger).
    ServiceControl(const addon_settings::AddonInfo& addon, std::string unit_path,
                   std::optional<unsigned long> uid, ProcessRunner run);

    /// is-active, is-enabled, show, la unidad en disco frente a params,
    /// linger y el último error.
    [[nodiscard]] ServiceStatus status(const addon_settings::UnitParams& params,
                                       const CancelToken* cancel = nullptr) const;

    /// Lleva el servicio a lo que dice Activado (al guardar), según cómo
    /// está ahora. enabled: escribe la unidad si falta o está vieja (una
    /// ajena solo con replace_foreign; si no, Confirm) y daemon-reload;
    /// enable --now si no corre o no está habilitado; si ya corría,
    /// restart (restart_bot) o reload. !enabled: disable --now si corre o
    /// está habilitado; si no, nada.
    [[nodiscard]] ServiceAction apply(const addon_settings::UnitParams& params, bool enabled,
                                      bool replace_foreign, bool restart_bot,
                                      const CancelToken* cancel = nullptr) const;
    /// disable --now.
    [[nodiscard]] ServiceAction disable(const CancelToken* cancel = nullptr) const;
    /// restart.
    [[nodiscard]] ServiceAction restart(const CancelToken* cancel = nullptr) const;
    /// reload, solo si está activo (al guardar).
    [[nodiscard]] ServiceAction reload_if_active(const CancelToken* cancel = nullptr) const;

private:
    [[nodiscard]] ProcessResult run(std::vector<std::string> argv,
                                    const CancelToken* cancel) const;
    [[nodiscard]] ProcessResult systemctl(std::vector<std::string> arguments,
                                          const CancelToken* cancel) const;
    [[nodiscard]] ServiceAction simple(std::vector<std::string> arguments, std::string_view done,
                                       std::string_view failed, const CancelToken* cancel) const;

    const addon_settings::AddonInfo& addon_;
    std::string unit_path_;
    std::optional<unsigned long> uid_;
    ProcessRunner run_;
};

/// Corre trabajos (de ServiceControl) en un hilo, en orden y de uno en uno,
/// sin FTXUI. Se usa desde un solo hilo (el de la interfaz); cada trabajo
/// devuelve lo que se hará en ese hilo, que llega por post.
class ServiceWorker {
public:
    using Task = std::function<void()>;
    /// Pasa una tarea al hilo de la interfaz; segura entre hilos.
    using Post = std::function<void(Task)>;
    /// Corre en el hilo de trabajo; su resultado corre en el de la interfaz.
    /// cancel se cancela al destruir el ServiceWorker.
    using Job = std::function<Task(const CancelToken& cancel)>;

    explicit ServiceWorker(Post post);
    /// Descarta lo pendiente, cancela el trabajo en curso y lo espera; lo
    /// que devuelva ya no se ejecuta.
    ~ServiceWorker();
    ServiceWorker(const ServiceWorker&) = delete;
    ServiceWorker& operator=(const ServiceWorker&) = delete;
    ServiceWorker(ServiceWorker&&) = delete;
    ServiceWorker& operator=(ServiceWorker&&) = delete;

    void push(Job job);
    /// Hay trabajos sin terminar (o su resultado aún no corre).
    [[nodiscard]] bool busy() const { return state_->pending > 0; }

private:
    void loop();

    /// Lo comparten las tareas en la cola de post, que pueden correr después
    /// del destructor. Solo se toca en el hilo de la interfaz.
    struct State {
        bool alive = true;
        std::size_t pending = 0;
    };
    Post post_;
    std::shared_ptr<State> state_;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<Job> jobs_;
    bool stopping_ = false;
    CancelToken cancel_;
    std::thread thread_;
};

} // namespace chatbot::cli

#endif // CHATBOT_CLI_SERVICE_CONTROL_H
