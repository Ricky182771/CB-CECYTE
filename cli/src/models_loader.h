#ifndef CHATBOT_CLI_MODELS_LOADER_H
#define CHATBOT_CLI_MODELS_LOADER_H

#include "chatbot/cancel_token.h"
#include "chatbot/config.h"
#include "chatbot/result.h"
#include "chatbot/transport.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace chatbot::cli {

/// Pide la lista de modelos (ChatClient::list_models) en un hilo de trabajo,
/// sin conocer FTXUI, igual que RequestRunner. Se usa solo desde un hilo (el
/// de la interfaz); el resultado regresa a ese hilo por la función post.
class ModelsLoader {
public:
    using Task = std::function<void()>;
    /// Pasa una tarea al hilo de la interfaz; se llama desde el hilo de
    /// trabajo: debe ser segura entre hilos.
    using Post = std::function<void(Task)>;
    /// Crea el transporte de cada petición (CurlTransport en la app).
    using TransportFactory = std::function<std::unique_ptr<Transport>()>;
    using OnDone = std::function<void(Result<std::vector<std::string>>)>;

    ModelsLoader(TransportFactory make_transport, Post post);
    /// Cancela lo que esté en curso y espera a que terminen los hilos (join).
    ~ModelsLoader();
    ModelsLoader(const ModelsLoader&) = delete;
    ModelsLoader& operator=(const ModelsLoader&) = delete;
    ModelsLoader(ModelsLoader&&) = delete;
    ModelsLoader& operator=(ModelsLoader&&) = delete;

    /// Pide los modelos con config (la del formulario, aunque no esté
    /// guardada). Si había una petición en curso, se cancela y su resultado
    /// se descarta, sin esperarla. on_done se ejecuta vía post, una vez,
    /// salvo que antes se cancele o se lance otra petición.
    void start(Config config, OnDone on_done);
    /// Cancela la petición en curso; su on_done ya no se ejecuta.
    void cancel();
    /// true desde start() hasta que se ejecuta su on_done (o cancel()).
    [[nodiscard]] bool busy() const { return state_->busy; }

private:
    /// Una petición: su hilo, su token y si el hilo ya terminó.
    struct Job {
        CancelToken token;
        std::atomic<bool> done{false};
        std::thread thread;
    };
    /// Une los hilos de peticiones viejas que ya terminaron.
    void reap();

    TransportFactory make_transport_;
    Post post_;
    /// Estado compartido con las tareas en la cola de post, que pueden
    /// ejecutarse después del destructor. Solo se toca en el hilo de la
    /// interfaz.
    struct State {
        std::uint64_t generation = 0; ///< Número de la petición vigente.
        bool busy = false;
    };
    std::shared_ptr<State> state_;
    std::unique_ptr<Job> current_;
    std::vector<std::unique_ptr<Job>> retired_; ///< Canceladas, quizá aún corriendo.
};

} // namespace chatbot::cli

#endif // CHATBOT_CLI_MODELS_LOADER_H
