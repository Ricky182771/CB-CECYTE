#include "chatbot/telegram/poller.h"

#include <algorithm>
#include <ctime>
#include <string>
#include <utility>
#include <vector>

namespace chatbot::telegram {
namespace {

/// Chats de grupos o canales que se anotan (una vez cada uno).
constexpr std::size_t kMaxIgnoredChats = 1000;

} // namespace

Poller::Poller(BotApi& api, PollerHandlers handlers, bot::Log& log, const CancelToken& shutdown,
               PollerOptions options)
    : api_(api), handlers_(std::move(handlers)), log_(log), shutdown_(shutdown),
      options_(options) {}

void Poller::log(bot::LogLevel level, const std::string& line) {
    try {
        log_.write(level, line);
    } catch (...) {
    }
}

bool Poller::wait(std::chrono::milliseconds duration) {
    Sleeper& sleeper = options_.sleeper != nullptr ? *options_.sleeper : real_sleeper_;
    return sleeper.sleep_for(duration, &shutdown_);
}

std::optional<ChatError> Poller::run() {
    while (!shutdown_.is_cancelled()) {
        if (std::optional<ChatError> fatal = poll_once()) {
            return fatal;
        }
    }
    return std::nullopt;
}

std::optional<ChatError> Poller::poll_once() {
    const Result<std::vector<Update>> result =
        api_.get_updates(offset_, options_.timeout, &shutdown_);
    if (result.is_error()) {
        const ChatError& error = result.error();
        if (error.kind == ErrorKind::Cancelled || shutdown_.is_cancelled()) {
            return std::nullopt;
        }
        if (error.kind == ErrorKind::Auth) {
            log(bot::LogLevel::Error, "telegram_token_invalido http=" +
                                          std::to_string(error.http_status));
            return error;
        }
        if (error.kind == ErrorKind::RateLimited) {
            (void)wait(std::chrono::duration_cast<std::chrono::milliseconds>(
                error.retry_after.value_or(std::chrono::seconds{1})));
            return std::nullopt;
        }
        // Sin red, Telegram caído, un 409 (otra copia del bot)…: se espera
        // cada vez más. Solo se anota el primer fallo.
        if (failures_ == 0) {
            offline_since_ = std::chrono::steady_clock::now();
            log(bot::LogLevel::Warning,
                "sin_conexion tipo=\"" + std::string{error_kind_label(error.kind)} +
                    "\" http=" + std::to_string(error.http_status));
        }
        ++failures_;
        const auto exponent = std::min<std::size_t>(failures_ - 1, 16);
        const std::chrono::seconds backoff =
            std::min(options_.max_backoff, std::chrono::seconds{std::int64_t{1} << exponent});
        (void)wait(backoff);
        return std::nullopt;
    }
    if (failures_ > 0) {
        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
                                 std::chrono::steady_clock::now() - offline_since_)
                                 .count();
        log(bot::LogLevel::Info, "conexion_recuperada fallos=" + std::to_string(failures_) +
                                     " segundos=" + std::to_string(seconds));
        failures_ = 0;
    }
    for (const Update& update : result.value()) {
        // El offset se recalcula con cada update: el siguiente getUpdates
        // confirma los que ya se vieron.
        if (update.update_id > 0) {
            offset_ = std::max(offset_, update.update_id + 1);
        }
        if (shutdown_.is_cancelled()) {
            break;
        }
        try {
            dispatch(update);
        } catch (...) {
            log(bot::LogLevel::Error, "update_fallido id=" + std::to_string(update.update_id));
        }
    }
    if (handlers_.idle) {
        handlers_.idle();
    }
    return std::nullopt;
}

void Poller::dispatch(const Update& update) {
    if (update.stop.has_value()) {
        const GenerationStopped& stop = *update.stop;
        if (stop.chat_type == "private" && handlers_.stop) {
            handlers_.stop(stop.chat_id, stop.draft_id);
        }
        return;
    }
    if (!update.message.has_value()) {
        return; // Otro tipo de update: solo cuenta para el offset.
    }
    const ChatMessage& message = *update.message;
    if (message.chat_type != "private") {
        // Solo chats privados: en grupos y canales el bot no responde.
        if (ignored_chats_.size() < kMaxIgnoredChats &&
            ignored_chats_.insert(message.chat_id).second) {
            log(bot::LogLevel::Info, "chat_no_privado chat=" + std::to_string(message.chat_id));
        }
        return;
    }
    if (message.from_bot || message.from_id <= 0) {
        return;
    }
    bot::IncomingMessage incoming;
    incoming.platform = "telegram";
    incoming.chat = message.chat_id;
    incoming.user = message.from_id;
    incoming.text = message.text.value_or(std::string{});
    if (message.date > 0) {
        incoming.sent_at = std::chrono::system_clock::from_time_t(static_cast<std::time_t>(message.date));
    }
    if (handlers_.message) {
        handlers_.message(incoming);
    }
}

} // namespace chatbot::telegram
