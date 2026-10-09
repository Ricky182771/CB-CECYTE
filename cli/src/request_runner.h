#ifndef CHATBOT_CLI_REQUEST_RUNNER_H
#define CHATBOT_CLI_REQUEST_RUNNER_H

#include "chatbot/cancel_token.h"
#include "chatbot/chat_client.h"
#include "chatbot/result.h"
#include "chatbot/types.h"
#include "chatbot/web_search.h"

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
    /// Resultados de una búsqueda que sí pasan al modelo: ya recortados con
    /// trim_search_response, y el bloque de format_search_context que quedó
    /// como content del último mensaje User.
    struct SearchContext {
        SearchResponse response;
        std::string block;
    };
    using OnSearchDone = std::function<void(SearchContext)>;

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

    /// Lanza una búsqueda web y, si trae resultados, la petición. Devuelve
    /// false si ya hay una en curso. history debe terminar en el mensaje User
    /// del comando ("/buscar …"): su content se reemplaza por el bloque de
    /// format_search_context(resultados, today, nonce), con un nonce nuevo;
    /// no se agrega otro mensaje. La búsqueda y la respuesta usan el mismo
    /// CancelToken: cancel() funciona igual en las dos.
    ///
    /// Un solo canal para los errores, on_done: si la búsqueda falla, se
    /// cancela o no trae resultados, no se llama al modelo y on_done recibe
    /// el error de la búsqueda tal cual (o NoSearchResults con kNoSearchResults),
    /// con dropped 0. on_search_done solo se llama si hay resultados, antes
    /// del primer delta; desde ahí todo sigue igual que en start(). El
    /// proveedor se comparte con el hilo de trabajo: quien llama puede
    /// reemplazar el suyo en cualquier momento.
    bool start_with_search(std::vector<Message> history, std::string query,
                           std::shared_ptr<SearchProvider> search_provider, std::string today,
                           OnSearchDone on_search_done, OnDelta on_delta, OnDone on_done);

    /// Cancela la petición en curso, si la hay. Mientras busy() sea true, la
    /// cancelación siempre gana: aunque la respuesta ya haya terminado en el
    /// hilo de trabajo, on_done recibe ErrorKind::Cancelled.
    void cancel();

    /// true desde start() hasta que se ejecuta su on_done.
    [[nodiscard]] bool busy() const;

private:
    /// Estado compartido con las tareas que quedan en la cola de post.
    struct State {
        bool busy = false;             ///< Solo se toca en el hilo de la interfaz.
        bool cancel_requested = false; ///< cancel() en la petición actual (hilo de la interfaz).
    };

    /// Prepara una petición nueva (join del hilo anterior, token nuevo,
    /// busy) y corre work en el hilo de trabajo. false si ya hay una en curso.
    bool launch(std::function<void(const CancelToken&)> work);

    /// En el hilo de trabajo: recorte del historial, petición, reintento por
    /// contexto y aviso final (on_done vía post). Común a start() y a
    /// start_with_search().
    static void complete(ChatClient& client, std::size_t limit, const Post& post,
                         const std::shared_ptr<State>& state, const CancelToken& token,
                         const std::vector<Message>& history, const OnDelta& on_delta,
                         const OnDone& on_done);

    /// Manda on_done al hilo de la interfaz. Allí, si el usuario canceló
    /// después de que el hilo terminó bien, la cancelación gana.
    static void post_done(const Post& post, const std::shared_ptr<State>& state,
                          const OnDone& on_done, Result<CompletionInfo> result,
                          std::size_t dropped);

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
