#ifndef CHATBOT_RPC_DISPATCHER_H
#define CHATBOT_RPC_DISPATCHER_H

#include "entry_diff.h"
#include "models_loader.h"
#include "protocol.h"
#include "provider_settings.h"
#include "session.h"
#include "telegram_addon.h"

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
    /// Crea el addon al recibir initialize (con el mismo post protegido).
    using AddonsFactory =
        std::function<std::unique_ptr<cli::TelegramAddon>(cli::ServiceWorker::Post)>;

    /// post: el de main (mete la tarea en la cola del hilo principal); las
    /// tareas que lleguen después de destruir la sesión no se ejecutan.
    /// models_transport: el transporte de cada models.list. make_addons: el
    /// TelegramAddon de addons.* y service.*.
    Dispatcher(ServerInfo info, SessionFactory make_session,
               cli::ModelsLoader::TransportFactory models_transport, AddonsFactory make_addons,
               cli::Session::Post post, Write write, Now now);
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
    /// Cancela y destruye la sesión y el addon (join); las peticiones
    /// pendientes no se responden.
    void close_session();
    /// El handle es el vigente; si no, responde UnknownConversation.
    [[nodiscard]] bool check_handle(const Json& params, const Json& id,
                                    bool has_id);
    void start_conversation(); ///< Handle nuevo y estado de la conversación de cero.

    /// Un SendResult que no es Sent, como los errores de chat.send; empty es
    /// el texto de Empty.
    void fail_send(cli::SendResult result, const Json& id, bool has_id, std::string_view empty);
    /// blocks.save y conversation.export: {path, message}, -32005 o -32007.
    void reply_file(const cli::FileOutcome& file, const Json& id, bool has_id);
    /// conversations.list, commands.list y commands.run.
    [[nodiscard]] Json conversation_list() const;
    [[nodiscard]] Json command_list() const;
    void run_command(const std::string& text, const Json& id, bool has_id);

    /// settings.* y models.list (rpc/PROTOCOL.md, sección 6.7).
    void settings(const std::string& method, const Json* params, const Json& id, bool has_id);
    /// provider_form con el borrador params.draft aplicado; si no se puede,
    /// responde el error y nullopt.
    [[nodiscard]] std::optional<cli::ProviderSettings> provider_draft(const Json& params,
                                                                      const Json& id, bool has_id);
    void models_list(const cli::ProviderSettings& form, const Json& id, bool has_id);
    /// El models.list pendiente responde -32800.
    void cancel_models();

    /// addons.* y service.* (rpc/PROTOCOL.md, sección 6.8).
    void addons(const std::string& method, const Json* params, const Json& id, bool has_id);
    void service(const std::string& method, const Json* params, const Json& id, bool has_id);

    /// Peticiones largas (models.list, addons.check_install, service.* e
    /// invited.remove): cada una recibe exactamente una respuesta, la suya o
    /// -32800 si se canceló; shutdown y EOF las olvidan sin responder. Una
    /// cancelada no se interrumpe: su resultado se descarta.

    /// La clave de una petición larga (una notificación no queda pendiente).
    [[nodiscard]] std::uint64_t track(const Json& id, bool has_id);
    /// El id si sigue pendiente; lo quita.
    std::optional<Json> untrack(std::uint64_t key);
    /// Al terminar una petición larga: vacía el delta, respond(id) si sigue
    /// pendiente y, con service_changed, service.changed (también si se
    /// canceló).
    void finish_long(std::uint64_t key, bool service_changed,
                     const std::function<void(const Json&)>& respond);

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
    cli::ModelsLoader::TransportFactory models_transport_;
    AddonsFactory make_addons_;
    cli::Session::Post post_;
    Write write_;
    Now now_;
    State state_ = State::Waiting;
    std::optional<int> exit_code_;
    /// Vive lo mismo que la sesión: las tareas de su post solo corren si sigue.
    std::shared_ptr<int> alive_;
    std::unique_ptr<cli::Session> session_;
    /// Usan el post protegido de la sesión; se destruyen antes que ella.
    std::unique_ptr<cli::ModelsLoader> models_;
    std::unique_ptr<cli::TelegramAddon> telegram_;
    struct Pending {
        std::uint64_t key = 0;
        Json id;
    };
    std::vector<Pending> pending_;
    std::uint64_t next_key_ = 0;
    /// La clave del models.list en curso (nullopt si no hay).
    std::optional<std::uint64_t> models_key_;
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
