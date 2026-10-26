#ifndef CHATBOT_RPC_DISPATCHER_H
#define CHATBOT_RPC_DISPATCHER_H

#include "entry_diff.h"
#include "protocol.h"
#include "session.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace chatbot::rpc {

/// Ventana en la que se juntan los deltas de una respuesta en un solo
/// conversation.entry_delta.
inline constexpr std::chrono::milliseconds kDeltaWindow{30};

/// Lo que el servidor sabe al arrancar; lo arma main (las pruebas, a mano).
struct ServerInfo {
    std::string version;  ///< server.version (PROJECT_VERSION).
    std::string platform; ///< "linux" | "windows".
    bool portable = false;
    std::optional<std::string> config_path;   ///< null si no se pudo resolver.
    std::optional<std::string> conversations_path;
    std::optional<std::string> downloads_path;
    std::vector<std::string> env_locks; ///< Las CHAT_* definidas, por nombre.
    /// El arranque falló (modo portable o config.json): el texto que la TUI
    /// imprime al salir. initialize responde StartupFailed con él.
    std::optional<std::string> startup_error;
};

/// Peticiones de JSON-RPC → Session → respuestas y eventos. Sin hilos, sin
/// stdio y sin reloj real: main le pasa cada línea, cada tarea terminada y
/// el paso del tiempo, y todo lo que sale va por write, en orden.
///
/// Orden: la respuesta a una petición sale antes que los eventos que
/// provocó, y antes de cualquier respuesta o evento se vacía el delta
/// pendiente.
class Dispatcher {
public:
    using TimePoint = std::chrono::steady_clock::time_point;
    using Write = std::function<void(const Json&)>;
    using Now = std::function<TimePoint()>;
    /// Crea la sesión al recibir initialize (con el post ya protegido).
    using SessionFactory =
        std::function<std::unique_ptr<cli::Session>(cli::Session::Post, cli::Session::OnFinished)>;

    /// post: el de main (mete la tarea en la cola del hilo principal); las
    /// tareas que lleguen después de destruir la sesión no se ejecutan.
    Dispatcher(ServerInfo info, SessionFactory make_session, cli::Session::Post post, Write write,
               Now now);
    ~Dispatcher();

    Dispatcher(const Dispatcher&) = delete;
    Dispatcher& operator=(const Dispatcher&) = delete;
    Dispatcher(Dispatcher&&) = delete;
    Dispatcher& operator=(Dispatcher&&) = delete;

    /// Una línea completa de stdin.
    void on_line(std::string_view line);
    /// Una línea que pasó de kMaxLineBytes: -32600 con id null.
    void on_oversized_line();
    /// Después de cada tarea que ejecutó el hilo principal.
    void after_task();
    /// Vacía el delta pendiente si ya pasó su ventana.
    void flush_due();
    /// Cuándo hay que llamar flush_due, si hay un delta pendiente.
    [[nodiscard]] std::optional<TimePoint> next_deadline() const;
    /// Fin de stdin (o no se pudo escribir): cancela y destruye la sesión
    /// (join), sin escribir nada.
    void on_eof();
    /// Llegó exit: el código de salida (0 si antes hubo shutdown, 1 si no).
    /// Quien llama cierra con on_eof y sale.
    [[nodiscard]] std::optional<int> exit_code() const { return exit_code_; }

private:
    enum class State { Waiting, Running, ShutDown };

    /// Atiende un mensaje ya interpretado; id null = notificación.
    void dispatch(const std::string& method, const Json* params,
                  const Json& id, bool has_id);
    void reply(const Json& id, bool has_id, const Json& result);
    void fail(const Json& id, bool has_id, RpcError code,
              const Json& data = nullptr);

    void initialize(const Json* params, const Json& id, bool has_id);
    void shutdown(const Json& id, bool has_id);
    /// Cancela y destruye la sesión (join).
    void close_session();
    /// El handle es el vigente; si no, responde UnknownConversation.
    [[nodiscard]] bool check_handle(const Json& params, const Json& id,
                                    bool has_id);
    void start_conversation(); ///< Handle nuevo y estado de la conversación de cero.

    [[nodiscard]] Phase current_phase() const;
    [[nodiscard]] Json snapshot() const;
    [[nodiscard]] Json status() const;
    /// Compara la conversación con lo ya emitido y emite los eventos. Sin
    /// flush, un cambio que solo es texto agregado espera kDeltaWindow.
    void sync(bool flush);
    /// Evento de la conversación vigente: agrega conversation y rev.
    void emit_conversation(std::string_view method, Json params);

    ServerInfo info_;
    SessionFactory make_session_;
    cli::Session::Post post_;
    Write write_;
    Now now_;
    State state_ = State::Waiting;
    std::optional<int> exit_code_;
    /// Vive lo mismo que la sesión: las tareas de su post solo corren si sigue.
    std::shared_ptr<int> alive_;
    std::unique_ptr<cli::Session> session_;
    std::optional<cli::Session::Finished> finished_; ///< Llegó en la última tarea.
    int handle_number_ = 0;
    std::string handle_;
    std::uint64_t rev_ = 0;
    EntriesSeen seen_;
    Phase phase_ = Phase::Idle;
    std::optional<TimePoint> pending_since_; ///< Primer delta que aún no sale.
};

} // namespace chatbot::rpc

#endif // CHATBOT_RPC_DISPATCHER_H
