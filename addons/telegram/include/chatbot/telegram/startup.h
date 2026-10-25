#ifndef CHATBOT_TELEGRAM_STARTUP_H
#define CHATBOT_TELEGRAM_STARTUP_H

#include "chatbot/telegram/api.h"
#include "chatbot/telegram/settings.h"

#include "chatbot/bot/types.h"

#include "chatbot/cancel_token.h"
#include "chatbot/result.h"
#include "chatbot/sleeper.h"

#include <chrono>
#include <optional>
#include <string>
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

/// Crea la carpeta de datos del bot (700 si es nueva) y prueba que se pueda
/// escribir en ella con un archivo que borra enseguida (ahí van
/// invitados.json y su candado). Error Config con la ruta y la causa.
[[nodiscard]] std::optional<ChatError> prepare_data_dir(const std::string& dir);

/// De dónde salió el error que detiene el bot.
enum class FailureSource {
    Telegram, ///< connect_to_telegram o el Poller.
    DataDir,  ///< prepare_data_dir.
    Invited,  ///< load_invited.
};

/// El código de salida de un error que detiene el bot (settings.h):
///   Telegram: Auth (no acepta el token, al arrancar o ya corriendo) → 78
///   DataDir: siempre → 73
///   Invited: BadResponse (invitados.json dañado) → 65
///   lo demás → 1, el único con el que systemd reinicia.
[[nodiscard]] int exit_code_for(const ChatError& error,
                                FailureSource source = FailureSource::Telegram);

} // namespace chatbot::telegram

#endif // CHATBOT_TELEGRAM_STARTUP_H
