#ifndef CHATBOT_TELEGRAM_OUTBOX_H
#define CHATBOT_TELEGRAM_OUTBOX_H

#include "chatbot/telegram/api.h"

#include "chatbot/bot/outbox.h"
#include "chatbot/bot/types.h"

#include "chatbot/cancel_token.h"
#include "chatbot/sleeper.h"

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace chatbot::telegram {

struct OutboxOptions {
    /// true: el registro de un envío rechazado lleva la descripción de
    /// Telegram (puede citar el texto). Como BotConfig::log_error_details.
    bool log_error_details = false;
    /// Intentos de un mensaje (no de una vista previa) con 429, error de red
    /// o 5xx; esperando retry_after o 1 s, 2 s…
    int send_attempts = 3;
    /// "Escribiendo…" dura 5 s: se repite antes.
    std::chrono::milliseconds typing_every{4000};
    /// Una vista previa dura unos 30 s: si no cambió en este tiempo, se
    /// vuelve a mandar (p. ej. mientras el modelo piensa).
    std::chrono::milliseconds keep_draft_every{20000};
    /// Cada cuánto revisa el hilo de la plataforma.
    std::chrono::milliseconds tick{500};
    /// Espera entre intentos (nullptr: RealSleeper, que corta con el token).
    Sleeper* sleeper = nullptr;
};

/// Lo que sale hacia Telegram (bot::Outbox): Markdown → sendRichMessage con
/// to_rich_markdown; texto plano → sendMessage; vista previa →
/// sendRichMessageDraft (con draft_id = el ReplyId de Bot, can_stop y
/// keep_on_stop). Un hilo propio (start()) repite "escribiendo…" y la vista
/// previa que no ha cambiado. Antes de mandar un mensaje a un chat, su vista
/// previa se pausa (que no reaparezca la vieja encima de la respuesta) hasta
/// el siguiente update(). Todo se corta con shutdown. Segura entre hilos.
class TelegramOutbox final : public bot::Outbox {
public:
    TelegramOutbox(BotApi& api, bot::Log& log, const CancelToken& shutdown,
                   OutboxOptions options = {});
    /// stop().
    ~TelegramOutbox() override;

    TelegramOutbox(const TelegramOutbox&) = delete;
    TelegramOutbox& operator=(const TelegramOutbox&) = delete;
    TelegramOutbox(TelegramOutbox&&) = delete;
    TelegramOutbox& operator=(TelegramOutbox&&) = delete;

    [[nodiscard]] bot::MessageLimit message_limit(bot::TextFormat format) const override;
    bool send(bot::ChatId chat, std::string_view text, bot::TextFormat format) override;
    void set_typing(bot::ChatId chat, bool typing) override;
    std::unique_ptr<bot::Draft> open_draft(bot::ChatId chat, bot::ReplyId reply) override;

    /// Arranca el hilo que repite "escribiendo…" y las vistas previas.
    void start();
    /// Lo detiene y lo espera. Se puede llamar varias veces.
    void stop();

    /// Una vuelta de ese hilo con la hora now (las pruebas la llaman sin
    /// hilo).
    void tick(std::chrono::steady_clock::time_point now);

private:
    struct DraftState {
        std::mutex mutex; ///< Un envío de esta vista previa a la vez.
        bot::ChatId chat = 0;
        bot::ReplyId draft_id = 0;
        std::string payload; ///< Lo último que se mostró (ya saneado).
        std::chrono::steady_clock::time_point last_sent;
        std::chrono::steady_clock::time_point blocked_until; ///< Tras un 429.
        bool paused = false;
    };
    class Draft;

    /// Manda payload como vista previa (con state.mutex tomado).
    bool push_draft(DraftState& state, std::string payload,
                    std::chrono::steady_clock::time_point now);
    void close_draft(const std::shared_ptr<DraftState>& state);
    /// Con reintentos; false si no se pudo.
    bool deliver(bot::ChatId chat, std::string_view method,
                 const std::function<std::optional<ChatError>()>& attempt);
    void log_failure(bot::LogLevel level, bot::ChatId chat, std::string_view method,
                     const ChatError& error);
    bool wait(std::chrono::milliseconds duration);

    BotApi& api_;
    bot::Log& log_;
    const CancelToken& shutdown_;
    OutboxOptions options_;
    RealSleeper real_sleeper_;

    std::mutex mutex_;
    std::map<bot::ChatId, std::chrono::steady_clock::time_point> typing_;
    std::map<bot::ChatId, std::shared_ptr<DraftState>> drafts_;

    std::mutex thread_mutex_;
    std::unique_ptr<CancelToken> stop_;
    std::thread thread_;
};

} // namespace chatbot::telegram

#endif // CHATBOT_TELEGRAM_OUTBOX_H
