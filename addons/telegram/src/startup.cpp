#include "chatbot/telegram/startup.h"

#include <algorithm>
#include <cstdint>
#include <string>

namespace chatbot::telegram {

std::vector<BotCommandInfo> bot_commands(bool search_available) {
    std::vector<BotCommandInfo> commands{
        {"start", "Empezar (o entrar con un código de invitación)"},
        {"ayuda", "Ver qué puedo hacer"},
        {"nuevo", "Empezar una conversación nueva"},
    };
    if (search_available) {
        commands.push_back({"buscar", "Buscar en la web y responder con las fuentes"});
    }
    return commands;
}

Result<BotIdentity> connect_to_telegram(BotApi& api, bot::Log& log, const CancelToken& shutdown,
                                        Sleeper* sleeper, std::chrono::seconds max_backoff) {
    RealSleeper real_sleeper;
    Sleeper& waiter = sleeper != nullptr ? *sleeper : real_sleeper;
    for (unsigned failures = 0;; ++failures) {
        Result<BotIdentity> identity = api.get_me(&shutdown);
        if (identity.is_ok()) {
            if (failures > 0) {
                log.write(bot::LogLevel::Info,
                          "conexion_recuperada fallos=" + std::to_string(failures));
            }
            return identity;
        }
        const ChatError& error = identity.error();
        if (error.kind == ErrorKind::Cancelled || shutdown.is_cancelled()) {
            return ChatError{ErrorKind::Cancelled, 0, "cancelado", std::nullopt};
        }
        if (error.kind == ErrorKind::Auth) {
            return ChatError{ErrorKind::Auth, error.http_status,
                             "Telegram no aceptó el token (HTTP " +
                                 std::to_string(error.http_status) +
                                 "). Revisa que sea el que te dio @BotFather.",
                             std::nullopt};
        }
        if (failures == 0) {
            log.write(bot::LogLevel::Warning,
                      "sin_conexion tipo=\"" + std::string{error_kind_label(error.kind)} +
                          "\" http=" + std::to_string(error.http_status));
        }
        const std::chrono::seconds wait =
            error.kind == ErrorKind::RateLimited && error.retry_after.has_value()
                ? *error.retry_after
                : std::min(max_backoff,
                           std::chrono::seconds{std::int64_t{1} << std::min(failures, 16U)});
        if (!waiter.sleep_for(wait, &shutdown)) {
            return ChatError{ErrorKind::Cancelled, 0, "cancelado", std::nullopt};
        }
    }
}

} // namespace chatbot::telegram
