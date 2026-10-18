#ifndef CHATBOT_BOT_BOT_H
#define CHATBOT_BOT_BOT_H

#include "chatbot/bot/access.h"
#include "chatbot/bot/config.h"
#include "chatbot/bot/outbox.h"
#include "chatbot/bot/sessions.h"
#include "chatbot/bot/types.h"

#include "chatbot/cancel_token.h"
#include "chatbot/chat_client.h"
#include "chatbot/types.h"
#include "chatbot/web_search.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace chatbot::bot {

/// Crea un ChatClient para una respuesta. Se llama desde los hilos de
/// trabajo, una vez por respuesta: cada una tiene su cliente y su transporte.
using ClientFactory = std::function<std::unique_ptr<ChatClient>()>;
/// Crea el proveedor de búsqueda para un /buscar (igual que ClientFactory).
using SearchFactory = std::function<std::unique_ptr<SearchProvider>()>;

/// Lo que el bot necesita de afuera. outbox, clock y log deben vivir más
/// que el Bot.
struct BotDependencies {
    ClientFactory make_client;
    SearchFactory make_search; ///< Vacía: sin key de búsqueda, sin /buscar.
    Outbox& outbox;
    Clock& clock;
    Log& log;
};

/// La lógica común de los bots, sin red ni plataforma: acceso, comandos,
/// sesiones, límites, respuestas del modelo y errores. La plataforma le pasa
/// cada mensaje con handle() y recibe lo que sale por Outbox.
///
/// Las respuestas corren en max_concurrent_responses hilos de trabajo; las
/// que no caben esperan su turno en una fila, en orden de llegada (como
/// cada chat tiene a lo más una pendiente, la fila no pasa del número de
/// chats). shutdown() (o el destructor) cancela todo con un CancelToken
/// compartido y espera a los hilos: la latencia es la de la cancelación del
/// núcleo (~1 s con curl).
class Bot {
public:
    Bot(ValidBotConfig config, BotDependencies dependencies);
    /// shutdown().
    ~Bot();

    Bot(const Bot&) = delete;
    Bot& operator=(const Bot&) = delete;
    Bot(Bot&&) = delete;
    Bot& operator=(Bot&&) = delete;

    /// Atiende un mensaje: acceso, comando, límites. Los avisos salen de
    /// inmediato por Outbox desde este hilo; una pregunta al modelo se pone
    /// en la fila y la responde un hilo de trabajo. Nunca espera al modelo.
    /// Seguro entre hilos. Después de shutdown() no hace nada.
    void handle(const IncomingMessage& message);

    /// Borra las conversaciones inactivas (las ocupadas no). handle() ya lo
    /// hace con cada mensaje; la plataforma lo llama de vez en cuando para
    /// que se borren aunque nadie escriba.
    void expire_idle();

    /// Cancela las respuestas en curso, descarta las de la fila (sin
    /// mandarles nada) y espera a los hilos. Se puede llamar varias veces; una
    /// llamada que llega mientras otra apaga espera a que termine. Lo que
    /// faltaba mandar de una respuesta ya no se manda.
    void shutdown();

    /// Espera hasta que no haya respuestas en la fila ni en curso, o hasta
    /// timeout. Devuelve true si quedó sin trabajo. Para las pruebas y para
    /// un apagado ordenado de la plataforma.
    bool wait_idle(std::chrono::milliseconds timeout);

    /// Conversaciones en memoria.
    [[nodiscard]] std::size_t session_count() const;

private:
    /// Una pregunta para el modelo, en la fila.
    struct Job {
        ChatId chat = 0;
        UserId user = 0;
        std::string platform;
        std::string day;                  ///< Día en que contó para el tope diario.
        std::string text;                 ///< Normal: lo que escribió.
        std::optional<std::string> query; ///< /buscar: la consulta.
        std::vector<Message> history;     ///< Copia al aceptarlo.
    };

    void work();
    void run(const Job& job);
    /// Manda un texto partido (split_markdown o split_plain). Devuelve
    /// cuántos pedazos mandó, o nullopt si la plataforma rechazó uno (los
    /// que seguían ya no se mandan).
    std::optional<std::size_t> deliver(ChatId chat, std::string_view text, TextFormat format);
    /// Con user_content y answer, agrega el par al historial del chat (que
    /// sigue ocupado); sin ellos, libera el chat (busy = false).
    void release(ChatId chat, std::optional<std::string> user_content,
                 std::optional<std::string> answer);
    void log(LogLevel level, const std::string& line);
    [[nodiscard]] std::string today() const;

    const BotConfig config_;
    BotDependencies dependencies_;
    const MessageLimit limit_;

    mutable std::mutex mutex_;
    std::condition_variable work_ready_;
    std::condition_variable idle_;
    AccessControl access_;
    SessionStore sessions_;
    DailyCounter counter_;
    /// No autorizados que ya se anotaron, con sus intentos de código anotados.
    std::map<UserId, unsigned> denied_;
    std::deque<Job> queue_;
    std::size_t running_ = 0;
    bool stopping_ = false;

    /// Compartido por todas las respuestas: solo shutdown() lo cancela.
    CancelToken cancel_;
    std::once_flag shutdown_once_;
    std::vector<std::thread> workers_;
};

} // namespace chatbot::bot

#endif // CHATBOT_BOT_BOT_H
