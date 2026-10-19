#ifndef CHATBOT_BOT_OUTBOX_H
#define CHATBOT_BOT_OUTBOX_H

#include "chatbot/bot/types.h"

#include <memory>
#include <string_view>

namespace chatbot::bot {

/// Vista previa de una respuesta mientras el modelo la genera (Telegram:
/// sendRichMessageDraft). La abre Outbox::open_draft y Bot la destruye
/// cuando ya mandó la respuesta (o el error). Se usa desde un solo hilo de
/// trabajo, pero la plataforma puede tener el suyo (p. ej. para que la
/// vista no desaparezca): la implementación lo protege.
class Draft {
public:
    virtual ~Draft() = default;

    /// Antes del primer texto: un aviso corto de que el bot está trabajando
    /// ("Pensando…", "Buscando en la web…"). false si la plataforma lo
    /// rechazó.
    virtual bool status(std::string_view label) = 0;

    /// Lo que va de la respuesta y todavía no se mandó como mensaje
    /// (markdown del modelo, que cabe en message_limit(Markdown)). false si
    /// la plataforma lo rechazó (p. ej. markdown a medias): Bot sigue con la
    /// respuesta y lo intenta con el siguiente.
    virtual bool update(std::string_view markdown) = 0;
};

/// Lo que sale hacia la plataforma: la implementa cada addon (Telegram) y la
/// usa Bot. Se llama desde el hilo que recibe los mensajes (avisos
/// inmediatos) y desde los hilos de trabajo (respuestas): debe ser segura
/// entre hilos.
class Outbox {
public:
    virtual ~Outbox() = default;

    /// Tope de un mensaje en cada formato y cómo lo mide la plataforma
    /// (Telegram: texto plano, 4096 unidades UTF-16; Markdown, el mensaje
    /// enriquecido ya saneado: bytes y bloques). Bot los lee una vez, al
    /// construirse; measure y count_blocks se llaman desde varios hilos.
    [[nodiscard]] virtual MessageLimit message_limit(TextFormat format) const = 0;

    /// Manda un mensaje que ya cabe en message_limit(format) y no está
    /// vacío. Devuelve false si la plataforma lo rechazó o no se pudo
    /// mandar: con TextFormat::Markdown, Bot lo vuelve a intentar como texto
    /// plano (partido con message_limit(Plain), por si lo que falló fue la
    /// conversión del formato) y, si tampoco, deja de mandar el resto de esa
    /// respuesta y lo anota en el registro.
    virtual bool send(ChatId chat, std::string_view text, TextFormat format) = 0;

    /// "Escribiendo…": true cuando un hilo empieza a preparar la respuesta
    /// de ese chat y false al terminar, antes de mandarla (o el error). La
    /// plataforma lo repite mientras siga en true (en Telegram,
    /// sendChatAction dura 5 s).
    virtual void set_typing(ChatId chat, bool typing) = 0;

    /// Vista previa para la respuesta reply del chat, o nullptr si la
    /// plataforma no tiene (lo predeterminado: entonces Bot manda la
    /// respuesta completa al final, con "escribiendo…" mientras). Con vista
    /// previa, Bot no usa set_typing salvo que el primer status() falle. Si
    /// el usuario la detiene, la plataforma llama a Bot::stop(chat, reply).
    virtual std::unique_ptr<Draft> open_draft(ChatId chat, ReplyId reply) {
        (void)chat;
        (void)reply;
        return nullptr;
    }
};

} // namespace chatbot::bot

#endif // CHATBOT_BOT_OUTBOX_H
