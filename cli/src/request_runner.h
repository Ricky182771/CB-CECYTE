#ifndef CHATBOT_CLI_REQUEST_RUNNER_H
#define CHATBOT_CLI_REQUEST_RUNNER_H

#include "chatbot/cancel_token.h"
#include "chatbot/chat_client.h"
#include "chatbot/result.h"
#include "chatbot/types.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace chatbot::cli {

/// Ejecuta una petición de streaming a la vez en un hilo de trabajo, sin
/// conocer FTXUI. Se usa solo desde un hilo (el de la interfaz); los
/// resultados regresan a ese hilo por la función post.
class RequestRunner {
public:
    using Task = std::function<void()>;
    /// Pasa una tarea al hilo de la interfaz. En la app: screen.Post(task) +
    /// screen.PostEvent(Event::Custom); en las pruebas, una cola que vacía la
    /// prueba. Se llama desde el hilo de trabajo: debe ser segura entre hilos.
    using Post = std::function<void(Task)>;
    using OnDelta = std::function<void(std::string)>;
    using OnDone = std::function<void(Result<CompletionInfo>, std::size_t dropped)>;

    /// El cliente debe vivir más que el runner. history_limit_bytes se pasa a
    /// trim_history (0 = sin límite).
    RequestRunner(ChatClient& client, std::size_t history_limit_bytes, Post post);

    /// Cancela la petición en curso y espera a que termine el hilo (join).
    ~RequestRunner();

    RequestRunner(const RequestRunner&) = delete;
    RequestRunner& operator=(const RequestRunner&) = delete;
    RequestRunner(RequestRunner&&) = delete;
    RequestRunner& operator=(RequestRunner&&) = delete;

    /// Lanza la petición. Devuelve false si ya hay una en curso. on_delta y
    /// on_done siempre se ejecutan vía post, nunca desde el hilo de trabajo;
    /// on_done se llama exactamente una vez por petición, con los mensajes
    /// omitidos por el recorte del intento que se usó al final.
    bool start(std::vector<Message> history, OnDelta on_delta, OnDone on_done);

    /// Cancela la petición en curso, si la hay.
    void cancel();

    /// true desde start() hasta que se ejecuta su on_done.
    [[nodiscard]] bool busy() const;

private:
    /// Estado compartido con las tareas que quedan en la cola de post.
    struct State {
        bool busy = false; ///< Solo se toca en el hilo de la interfaz.
    };

    ChatClient& client_;
    std::size_t history_limit_bytes_;
    Post post_;
    std::shared_ptr<State> state_;
    /// Un token por petición. Vive hasta el siguiente start() o el
    /// destructor, que antes hacen join: cubre toda la vida del hilo.
    std::unique_ptr<CancelToken> token_;
    std::thread worker_;
};

} // namespace chatbot::cli

#endif // CHATBOT_CLI_REQUEST_RUNNER_H
