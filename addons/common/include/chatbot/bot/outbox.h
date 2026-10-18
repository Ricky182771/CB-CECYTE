#ifndef CHATBOT_BOT_OUTBOX_H
#define CHATBOT_BOT_OUTBOX_H

#include "chatbot/bot/types.h"

#include <string_view>

namespace chatbot::bot {

/// Lo que sale hacia la plataforma: la implementa cada addon (Telegram) y la
/// usa Bot. Se llama desde el hilo que recibe los mensajes (avisos
/// inmediatos) y desde los hilos de trabajo (respuestas): debe ser segura
/// entre hilos.
class Outbox {
public:
    virtual ~Outbox() = default;

    /// Tope de un mensaje y cómo lo mide la plataforma (Telegram: 4096
    /// unidades UTF-16). Bot lo lee una vez, al construirse; measure se llama
    /// desde varios hilos.
    [[nodiscard]] virtual MessageLimit message_limit() const = 0;

    /// Manda un mensaje que ya cabe en message_limit() y no está vacío.
    /// Devuelve false si la plataforma lo rechazó o no se pudo mandar: con
    /// TextFormat::Markdown, Bot lo vuelve a intentar como texto plano (por
    /// si lo que falló fue la conversión del formato) y, si tampoco, deja de
    /// mandar el resto de esa respuesta y lo anota en el registro.
    virtual bool send(ChatId chat, std::string_view text, TextFormat format) = 0;

    /// "Escribiendo…": true cuando un hilo empieza a preparar la respuesta
    /// de ese chat y false al terminar, antes de mandarla (o el error). La
    /// plataforma lo repite mientras siga en true (en Telegram,
    /// sendChatAction dura 5 s).
    virtual void set_typing(ChatId chat, bool typing) = 0;
};

} // namespace chatbot::bot

#endif // CHATBOT_BOT_OUTBOX_H
