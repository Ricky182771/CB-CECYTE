#ifndef CHATBOT_BOT_TYPES_H
#define CHATBOT_BOT_TYPES_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace chatbot::bot {

/// ID de un chat de la plataforma (en Telegram, chat.id: puede ser negativo
/// en los grupos).
using ChatId = std::int64_t;
/// ID de un usuario de la plataforma (en Telegram, from.id: siempre > 0).
using UserId = std::int64_t;
/// Identifica una respuesta en curso (Bot::stop). Bot los da seguidos desde
/// el que dé BotDependencies::first_reply, entre 1 y kMaxReplyId (en
/// Telegram es el draft_id, un Integer).
using ReplyId = std::int64_t;

/// Después de este, los ReplyId vuelven a 1 (2^31 - 1: cabe en un Integer de
/// Telegram).
inline constexpr ReplyId kMaxReplyId = 2147483647;

/// De dónde sale el primer ReplyId de un Bot. Un valor fuera de
/// [1, kMaxReplyId] se lleva a ese rango.
using ReplyIdSource = std::function<ReplyId()>;

/// Un ReplyId al azar en [1, kMaxReplyId] (std::random_device; si no hay
/// fuente de entropía, el reloj). Así un bot que se reinicia no repite los de
/// antes: Telegram ignora en silencio un draft_id que ya se detuvo en ese
/// chat.
[[nodiscard]] ReplyId random_reply_id();

/// value si está en [1, kMaxReplyId]; si no, otro de ese rango (nunca 0).
[[nodiscard]] ReplyId to_reply_range(ReplyId value);

/// Un mensaje que llegó a la plataforma, ya traducido. El texto y el nombre
/// nunca van al registro.
struct IncomingMessage {
    std::string platform;     ///< "telegram": solo para el registro.
    ChatId chat = 0;
    UserId user = 0;
    std::string display_name; ///< Nombre visible; no se usa para decidir nada.
    std::string text;         ///< Vacío si no era texto (foto, sticker…).
    /// Cuándo lo mandó el usuario, si la plataforma lo dice. Uno más viejo
    /// que BotConfig::stale_after (llegó tarde: el bot estaba apagado) no
    /// va al modelo.
    std::optional<std::chrono::system_clock::time_point> sent_at = std::nullopt;
};

/// Formato de un mensaje que sale: Markdown (la respuesta del modelo, que la
/// plataforma convierte a su formato) o texto plano (avisos, errores,
/// fuentes), que la plataforma manda tal cual.
enum class TextFormat { Markdown, Plain };

/// Cómo mide la plataforma un mensaje y cuánto acepta. measure debería
/// crecer con el texto y medir al menos 1 por carácter (así se parte en
/// pedazos grandes); vacía mide bytes UTF-8. Si no (p. ej. mide el texto ya
/// convertido al formato de la plataforma, que puede encogerse), los
/// pedazos siguen cabiendo, solo pueden salir más chicos. Con max_blocks >
/// 0, además, count_blocks(texto) no debe pasar de max_blocks (Telegram:
/// 500 bloques en un mensaje enriquecido).
struct MessageLimit {
    std::size_t max_size = 4096;
    std::function<std::size_t(std::string_view)> measure;
    std::size_t max_blocks = 0;
    std::function<std::size_t(std::string_view)> count_blocks = {};

    [[nodiscard]] std::size_t size_of(std::string_view text) const {
        return measure ? measure(text) : text.size();
    }

    [[nodiscard]] std::size_t blocks_of(std::string_view text) const {
        return count_blocks ? count_blocks(text) : 0;
    }
};

/// Unidades UTF-16 del texto UTF-8 (como mide Telegram): 2 por cada carácter
/// fuera del plano básico, 1 por los demás; cada byte inválido cuenta 1
/// (U+FFFD).
[[nodiscard]] std::size_t utf16_length(std::string_view utf8);

/// Reloj inyectable: las sesiones expiran y el tope diario cambia de día con
/// él, para probar sin esperar.
class Clock {
public:
    virtual ~Clock() = default;
    [[nodiscard]] virtual std::chrono::system_clock::time_point now() const = 0;
};

/// El reloj del sistema.
class SystemClock final : public Clock {
public:
    [[nodiscard]] std::chrono::system_clock::time_point now() const override {
        return std::chrono::system_clock::now();
    }
};

enum class LogLevel { Info, Warning, Error };

/// Registro del bot. Las líneas son "evento clave=valor …" con IDs, tiempos,
/// tamaños y errores; nunca el texto de los mensajes ni de las respuestas.
/// Se llama desde varios hilos: la implementación debe ser segura entre
/// hilos.
class Log {
public:
    virtual ~Log() = default;
    virtual void write(LogLevel level, std::string_view line) = 0;
};

} // namespace chatbot::bot

#endif // CHATBOT_BOT_TYPES_H
