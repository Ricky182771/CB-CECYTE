#ifndef CHATBOT_TELEGRAM_POLLER_H
#define CHATBOT_TELEGRAM_POLLER_H

#include "chatbot/telegram/api.h"

#include "chatbot/bot/types.h"

#include "chatbot/cancel_token.h"
#include "chatbot/error.h"
#include "chatbot/sleeper.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <set>

namespace chatbot::telegram {

/// A dónde van los updates (en producción, al bot::Bot).
struct PollerHandlers {
    std::function<void(const bot::IncomingMessage&)> message;
    std::function<void(bot::ChatId, bot::ReplyId)> stop;
    /// Después de cada respuesta de getUpdates (bot::Bot::expire_idle).
    std::function<void()> idle;
};

struct PollerOptions {
    std::chrono::seconds timeout = kPollTimeout;
    /// Espera tras un fallo: 1 s, 2 s, 4 s… hasta este tope.
    std::chrono::seconds max_backoff{60};
    /// nullptr: RealSleeper (se corta con el token de apagado).
    Sleeper* sleeper = nullptr;
};

/// El long polling: getUpdates con el offset recalculado después de cada
/// respuesta; los mensajes de chats privados (de personas) van a
/// handlers.message y "Detener" a handlers.stop; los de grupos y canales se
/// ignoran (cada chat se anota una vez). Sin red (Network, Timeout, Server…)
/// espera cada vez más, con tope, y anota una línea al perder la conexión y
/// otra al recuperarla; con 429, espera retry_after. Un token inválido
/// (Auth) termina con ese error.
class Poller {
public:
    Poller(BotApi& api, PollerHandlers handlers, bot::Log& log, const CancelToken& shutdown,
           PollerOptions options = {});

    /// Hasta el apagado (nullopt) o un error que no se arregla esperando.
    [[nodiscard]] std::optional<ChatError> run();

    /// Una vuelta: un getUpdates y lo que siga (o la espera tras un fallo).
    /// nullopt para seguir; el error si hay que salir. Para las pruebas.
    [[nodiscard]] std::optional<ChatError> poll_once();

    [[nodiscard]] std::int64_t offset() const { return offset_; }

private:
    void dispatch(const Update& update);
    void log(bot::LogLevel level, const std::string& line);
    bool wait(std::chrono::milliseconds duration);

    BotApi& api_;
    PollerHandlers handlers_;
    bot::Log& log_;
    const CancelToken& shutdown_;
    PollerOptions options_;
    RealSleeper real_sleeper_;

    std::int64_t offset_ = 0;
    std::size_t failures_ = 0; ///< Fallos seguidos (0: conectado).
    std::chrono::steady_clock::time_point offline_since_;
    std::set<bot::ChatId> ignored_chats_;
};

} // namespace chatbot::telegram

#endif // CHATBOT_TELEGRAM_POLLER_H
