#ifndef CHATBOT_TELEGRAM_STARTUP_H
#define CHATBOT_TELEGRAM_STARTUP_H

#include "chatbot/telegram/api.h"

#include "chatbot/bot/types.h"

#include "chatbot/cancel_token.h"
#include "chatbot/result.h"
#include "chatbot/sleeper.h"

#include <chrono>
#include <vector>

namespace chatbot::telegram {

/// Los comandos del menú de Telegram (setMyCommands), con descripciones en
/// español; /buscar solo si hay búsqueda.
[[nodiscard]] std::vector<BotCommandInfo> bot_commands(bool search_available);

/// Al arrancar: getMe hasta que conteste. Un token inválido (Auth) es un
/// error claro; sin red (el bot arrancó antes que el módem tras un apagón)
/// espera cada vez más, hasta max_backoff, y lo anota una vez. Con el
/// apagado, Cancelled.
[[nodiscard]] Result<BotIdentity> connect_to_telegram(BotApi& api, bot::Log& log,
                                                      const CancelToken& shutdown,
                                                      Sleeper* sleeper = nullptr,
                                                      std::chrono::seconds max_backoff =
                                                          std::chrono::seconds{60});

} // namespace chatbot::telegram

#endif // CHATBOT_TELEGRAM_STARTUP_H
