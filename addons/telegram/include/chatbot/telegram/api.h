#ifndef CHATBOT_TELEGRAM_API_H
#define CHATBOT_TELEGRAM_API_H

#include "chatbot/cancel_token.h"
#include "chatbot/error.h"
#include "chatbot/result.h"
#include "chatbot/transport.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace chatbot::telegram {

/// Servidor de la Bot API.
inline constexpr std::string_view kApiBase = "https://api.telegram.org";
/// Long polling: Telegram espera hasta esto antes de contestar sin updates.
inline constexpr std::chrono::seconds kPollTimeout{50};
/// Timeout de las demás peticiones.
inline constexpr std::chrono::seconds kRequestTimeout{30};

/// Crea un transporte HTTP (CurlTransport en producción). BotApi guarda los
/// que no está usando para reusar la conexión.
using TransportFactory = std::function<std::unique_ptr<Transport>()>;

/// Un mensaje que llegó (Message de la Bot API), solo con lo que se usa.
struct ChatMessage {
    std::int64_t chat_id = 0;
    std::string chat_type;      ///< "private", "group", "supergroup", "channel".
    std::int64_t from_id = 0;   ///< 0 si no hay remitente.
    bool from_bot = false;
    std::optional<std::string> text; ///< nullopt: no es texto (foto, sticker…).
    std::int64_t date = 0;      ///< Hora Unix en que se mandó.
};

/// El usuario presionó "Detener" en una vista previa
/// (stopped_message_generation, Bot API 10.3).
struct GenerationStopped {
    std::int64_t chat_id = 0;
    std::string chat_type;
    std::int64_t draft_id = 0; ///< Siempre > 0 (si no, el update es inválido).
};

/// Un update que no se pudo interpretar. Solo nombres fijos, nunca valores
/// del update: el Poller lo anota tal cual.
struct UpdateProblem {
    std::string type;   ///< "message", "stopped_message_generation" u "otro".
    std::string reason; ///< "chat_invalido", "draft_id_invalido", "update_id_invalido"…
};

/// Un update de getUpdates. Los de otros tipos llegan sin message ni stop
/// (solo cuentan para el offset). Un message o un
/// stopped_message_generation que no se pudo interpretar llega sin él y con
/// problem; sin update_id válido, con update_id 0 (no cuenta para el
/// offset). Los enteros pueden llegar como número o como cadena de dígitos.
struct Update {
    std::int64_t update_id = 0;
    std::optional<ChatMessage> message;
    std::optional<GenerationStopped> stop;
    std::optional<UpdateProblem> problem;
};

struct BotIdentity {
    std::int64_t id = 0;
    std::string username;
};

struct BotCommandInfo {
    std::string command;     ///< Sin '/': 1-32 letras minúsculas, dígitos o '_'.
    std::string description; ///< 1-256 caracteres.
};

/// true si el token tiene la forma de uno de @BotFather: dígitos, ':' y de
/// 20 a 100 caracteres A-Z, a-z, 0-9, '_' o '-'. Así nunca se arma una URL
/// con algo que no sea un token (/, ?, #, espacios).
[[nodiscard]] bool plausible_token(std::string_view token);

/// Cliente de la Bot API sobre el Transport del núcleo: POST JSON a
/// https://api.telegram.org/bot<token>/<método>. El token va en la ruta, así
/// que ningún error, aviso ni registro lo incluye: los mensajes del
/// transporte y de Telegram pasan por redact() y la URL de una redirección
/// (3xx) no se usa. Errores (ErrorKind):
/// - sin respuesta: Cancelled, Timeout o Network;
/// - 3xx: BadResponse, sin la URL;
/// - 400: InvalidRequest; 401, 403 y 404: Auth (token inválido o, al
///   mandar, el usuario bloqueó al bot); 409: Config (otro programa usa el
///   mismo token o hay un webhook); 429: RateLimited con
///   parameters.retry_after; 5xx: Server; lo demás o un cuerpo que no se
///   entiende: BadResponse.
/// Sin reintentos: los hace quien llama. Seguro entre hilos.
class BotApi {
public:
    BotApi(std::string token, TransportFactory transports, std::string base = std::string{kApiBase});

    [[nodiscard]] Result<BotIdentity> get_me(const CancelToken* cancel = nullptr);

    /// Long polling con timeout segundos; el transporte espera 15 s más.
    /// allowed_updates: solo message y stopped_message_generation.
    [[nodiscard]] Result<std::vector<Update>> get_updates(std::int64_t offset,
                                                          std::chrono::seconds timeout,
                                                          const CancelToken* cancel = nullptr);

    /// Texto plano (sin parse_mode), sin vista previa de enlaces.
    [[nodiscard]] std::optional<ChatError> send_message(std::int64_t chat, std::string_view text,
                                                        const CancelToken* cancel = nullptr);

    /// Mensaje enriquecido con rich_message.markdown (ya saneado) y
    /// skip_entity_detection: solo son enlaces los que ya van en el
    /// markdown.
    [[nodiscard]] std::optional<ChatError> send_rich_message(std::int64_t chat,
                                                             std::string_view markdown,
                                                             const CancelToken* cancel = nullptr);

    /// Vista previa (solo chats privados) con can_stop y keep_on_stop.
    [[nodiscard]] std::optional<ChatError> send_rich_message_draft(
        std::int64_t chat, std::int64_t draft_id, std::string_view markdown,
        const CancelToken* cancel = nullptr);

    /// "typing": dura 5 s o hasta el siguiente mensaje.
    [[nodiscard]] std::optional<ChatError> send_chat_action(std::int64_t chat,
                                                            const CancelToken* cancel = nullptr);

    [[nodiscard]] std::optional<ChatError> set_my_commands(
        const std::vector<BotCommandInfo>& commands, const CancelToken* cancel = nullptr);

    /// text sin el token (ni su forma con ':' codificado): "<token>".
    [[nodiscard]] std::string redact(std::string_view text) const;

private:
    /// POST de body a método; devuelve "result" o el error.
    [[nodiscard]] Result<std::string> call(std::string_view method, const std::string& body,
                                           std::chrono::milliseconds timeout,
                                           const CancelToken* cancel);

    std::unique_ptr<Transport> acquire();
    void release(std::unique_ptr<Transport> transport);

    std::string token_;
    TransportFactory transports_;
    std::string base_;
    std::mutex pool_mutex_;
    std::vector<std::unique_ptr<Transport>> pool_;
};

} // namespace chatbot::telegram

#endif // CHATBOT_TELEGRAM_API_H
