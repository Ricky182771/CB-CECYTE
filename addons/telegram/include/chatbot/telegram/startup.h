#ifndef CHATBOT_TELEGRAM_STARTUP_H
#define CHATBOT_TELEGRAM_STARTUP_H

#include "chatbot/telegram/api.h"
#include "chatbot/telegram/outbox.h"
#include "chatbot/telegram/settings.h"

#include "chatbot/bot/bot.h"
#include "chatbot/bot/types.h"

#include "chatbot/cancel_token.h"
#include "chatbot/result.h"
#include "chatbot/sleeper.h"
#include "chatbot/transport.h"

#include <chrono>
#include <functional>
#include <memory>
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

/// El cliente del modelo de settings, uno por respuesta (make_transport:
/// CurlTransport; en las pruebas, falsos).
[[nodiscard]] bot::ClientFactory model_factory(const TelegramSettings& settings,
                                               TransportFactory make_transport);
/// La búsqueda con la key de settings; vacía si no hay key (sin /buscar).
[[nodiscard]] bot::SearchFactory search_factory(const TelegramSettings& settings,
                                                TransportFactory make_transport);

/// Lo que cambia una recarga.
struct ReloadTargets {
    bot::Bot& bot;
    TelegramOutbox& outbox;
    BotApi& api;
    const CancelToken& shutdown; ///< Corta setMyCommands.
    TransportFactory make_transport;
};

enum class ReloadResult {
    Applied, ///< config_recargada.
    Kept,    ///< Algo no es válido: sigue la de antes (el error, en el registro).
    Disable, ///< enabled: false: hay que apagar el bot y salir con 0.
};

/// SIGHUP: vuelve a leer config.json, credentials.json e invitados.json y,
/// si todo es válido, lo aplica en caliente (Bot::reconfigure,
/// log_error_details del outbox y setMyCommands si /buscar aparece o
/// desaparece). El token no cambia hasta reiniciar (requiere_reinicio
/// campo=token). current es lo cargado; se actualiza al aplicar. Las líneas
/// del registro no llevan valores.
[[nodiscard]] ReloadResult reload_settings(const SettingsPaths& paths, const EnvLookup& env,
                                           TelegramSettings& current,
                                           const ReloadTargets& targets, bot::Log& log);

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
