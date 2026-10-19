#include "chatbot/telegram/outbox.h"

#include "chatbot/telegram/rich_markdown.h"

#include <utility>
#include <vector>

namespace chatbot::telegram {
namespace {

bool is_blank(std::string_view text) {
    return text.find_first_not_of(" \t\r\n") == std::string_view::npos;
}

bool retryable(ErrorKind kind) {
    return kind == ErrorKind::RateLimited || kind == ErrorKind::Network ||
           kind == ErrorKind::Timeout || kind == ErrorKind::Server;
}

} // namespace

/// La vista previa de una respuesta (bot::Draft).
class TelegramOutbox::Draft final : public bot::Draft {
public:
    Draft(TelegramOutbox& outbox, std::shared_ptr<DraftState> state)
        : outbox_(outbox), state_(std::move(state)) {}
    ~Draft() override { outbox_.close_draft(state_); }
    Draft(const Draft&) = delete;
    Draft& operator=(const Draft&) = delete;
    Draft(Draft&&) = delete;
    Draft& operator=(Draft&&) = delete;

    bool status(std::string_view label) override {
        // El aviso lo arma el bot (no viene del modelo): <tg-thinking> solo
        // existe en las vistas previas.
        return push("<tg-thinking>" + to_rich_markdown(label) + "</tg-thinking>");
    }

    bool update(std::string_view markdown) override {
        std::string rich = to_rich_markdown(markdown);
        if (is_blank(rich)) {
            return true; // Nada que mostrar todavía.
        }
        return push(std::move(rich));
    }

private:
    bool push(std::string payload) {
        const std::lock_guard<std::mutex> lock(state_->mutex);
        return outbox_.push_draft(*state_, std::move(payload), std::chrono::steady_clock::now());
    }

    TelegramOutbox& outbox_;
    std::shared_ptr<DraftState> state_;
};

TelegramOutbox::TelegramOutbox(BotApi& api, bot::Log& log, const CancelToken& shutdown,
                               OutboxOptions options)
    : api_(api), log_(log), shutdown_(shutdown), options_(options) {}

TelegramOutbox::~TelegramOutbox() {
    stop();
}

bot::MessageLimit TelegramOutbox::message_limit(bot::TextFormat format) const {
    return format == bot::TextFormat::Markdown ? rich_message_limit() : plain_message_limit();
}

bool TelegramOutbox::wait(std::chrono::milliseconds duration) {
    Sleeper& sleeper = options_.sleeper != nullptr ? *options_.sleeper : real_sleeper_;
    return sleeper.sleep_for(duration, &shutdown_);
}

void TelegramOutbox::log_failure(bot::LogLevel level, bot::ChatId chat, std::string_view method,
                                 const ChatError& error) {
    std::string line = "telegram_error metodo=" + std::string{method} +
                       " chat=" + std::to_string(chat) + " http=" +
                       std::to_string(error.http_status) + " tipo=\"" +
                       error_kind_label(error.kind) + "\"";
    if (options_.log_error_details) {
        // Ya sin el token (BotApi::redact) y en una línea.
        std::string detail = error.message;
        for (char& c : detail) {
            c = c == '"' ? '\'' : c;
        }
        line += " detalle=\"" + detail + "\"";
    }
    try {
        log_.write(level, line);
    } catch (...) {
    }
}

bool TelegramOutbox::deliver(bot::ChatId chat, std::string_view method,
                             const std::function<std::optional<ChatError>()>& attempt) {
    for (int i = 1;; ++i) {
        const std::optional<ChatError> error = attempt();
        if (!error.has_value()) {
            return true;
        }
        if (error->kind == ErrorKind::Cancelled || shutdown_.is_cancelled()) {
            return false;
        }
        if (!retryable(error->kind) || i >= options_.send_attempts) {
            // Un mensaje enriquecido rechazado (400) no es grave: Bot lo
            // manda como texto plano.
            const bool expected = error->kind == ErrorKind::InvalidRequest &&
                                  method == "sendRichMessage";
            log_failure(expected ? bot::LogLevel::Warning : bot::LogLevel::Error, chat, method,
                        *error);
            return false;
        }
        const std::chrono::milliseconds pause =
            error->retry_after.has_value()
                ? std::chrono::duration_cast<std::chrono::milliseconds>(*error->retry_after)
                : std::chrono::milliseconds{1000 * i};
        if (!wait(pause)) {
            return false; // Apagando.
        }
    }
}

bool TelegramOutbox::send(bot::ChatId chat, std::string_view text, bot::TextFormat format) {
    // La vista previa vieja no debe volver a aparecer encima del mensaje.
    std::shared_ptr<DraftState> draft;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (const auto found = drafts_.find(chat); found != drafts_.end()) {
            draft = found->second;
        }
    }
    if (draft) {
        const std::lock_guard<std::mutex> lock(draft->mutex);
        draft->paused = true;
    }
    if (format == bot::TextFormat::Markdown) {
        const std::string rich = to_rich_markdown(text);
        if (is_blank(rich)) {
            return false; // Nada que mandar enriquecido: Bot lo manda plano.
        }
        return deliver(chat, "sendRichMessage",
                       [&] { return api_.send_rich_message(chat, rich, &shutdown_); });
    }
    return deliver(chat, "sendMessage", [&] { return api_.send_message(chat, text, &shutdown_); });
}

void TelegramOutbox::set_typing(bot::ChatId chat, bool typing) {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (!typing) {
            typing_.erase(chat);
            return;
        }
        typing_[chat] = std::chrono::steady_clock::now();
    }
    (void)api_.send_chat_action(chat, &shutdown_); // Opcional: si falla, nada.
}

std::unique_ptr<bot::Draft> TelegramOutbox::open_draft(bot::ChatId chat, bot::ReplyId reply) {
    if (chat <= 0) {
        return nullptr; // Las vistas previas solo existen en chats privados.
    }
    auto state = std::make_shared<DraftState>();
    state->chat = chat;
    state->draft_id = reply;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        drafts_[chat] = state;
    }
    return std::make_unique<Draft>(*this, std::move(state));
}

bool TelegramOutbox::push_draft(DraftState& state, std::string payload,
                                std::chrono::steady_clock::time_point now) {
    state.paused = false;
    state.payload = std::move(payload);
    if (now < state.blocked_until) {
        return true; // Esperando un 429: el hilo la manda cuando se pueda.
    }
    const std::optional<ChatError> error =
        api_.send_rich_message_draft(state.chat, state.draft_id, state.payload, &shutdown_);
    if (error.has_value()) {
        if (error->kind == ErrorKind::RateLimited) {
            state.blocked_until =
                now + (error->retry_after.has_value()
                           ? std::chrono::duration_cast<std::chrono::milliseconds>(*error->retry_after)
                           : std::chrono::milliseconds{1000});
        }
        // Lo anota Bot (vistas_rechazadas); aquí no, para que el markdown a
        // medias no llene el registro.
        return false;
    }
    state.last_sent = now;
    return true;
}

void TelegramOutbox::close_draft(const std::shared_ptr<DraftState>& state) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (const auto found = drafts_.find(state->chat);
        found != drafts_.end() && found->second == state) {
        drafts_.erase(found);
    }
}

void TelegramOutbox::tick(std::chrono::steady_clock::time_point now) {
    std::vector<bot::ChatId> typing;
    std::vector<std::shared_ptr<DraftState>> drafts;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [chat, last] : typing_) {
            if (now - last >= options_.typing_every) {
                last = now;
                typing.push_back(chat);
            }
        }
        for (const auto& [chat, state] : drafts_) {
            drafts.push_back(state);
        }
    }
    for (const bot::ChatId chat : typing) {
        if (shutdown_.is_cancelled()) {
            return;
        }
        (void)api_.send_chat_action(chat, &shutdown_);
    }
    for (const std::shared_ptr<DraftState>& state : drafts) {
        if (shutdown_.is_cancelled()) {
            return;
        }
        const std::lock_guard<std::mutex> lock(state->mutex);
        const bool stale = now - state->last_sent >= options_.keep_draft_every;
        const bool unblocked = state->blocked_until != std::chrono::steady_clock::time_point{} &&
                               now >= state->blocked_until && state->last_sent < state->blocked_until;
        if (state->paused || state->payload.empty() || now < state->blocked_until ||
            !(stale || unblocked)) {
            continue;
        }
        (void)push_draft(*state, state->payload, now);
    }
}

void TelegramOutbox::start() {
    const std::lock_guard<std::mutex> lock(thread_mutex_);
    if (thread_.joinable()) {
        return;
    }
    stop_ = std::make_unique<CancelToken>();
    thread_ = std::thread{[this, stop = stop_.get()] {
        while (!stop->wait_for(options_.tick)) {
            try {
                tick(std::chrono::steady_clock::now());
            } catch (...) {
                // El hilo nunca debe tumbar al bot.
            }
        }
    }};
}

void TelegramOutbox::stop() {
    const std::lock_guard<std::mutex> lock(thread_mutex_);
    if (stop_) {
        stop_->cancel();
    }
    if (thread_.joinable()) {
        thread_.join();
    }
}

} // namespace chatbot::telegram
