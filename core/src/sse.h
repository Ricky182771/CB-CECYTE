#ifndef CHATBOT_SRC_SSE_H
#define CHATBOT_SRC_SSE_H

#include "chatbot/error.h"
#include "chatbot/result.h"

#include <deque>
#include <optional>
#include <string>

namespace chatbot {
namespace sse {

/// Estado del parser de eventos SSE (subconjunto usado por OpenAI/NIM,
/// decisión del usuario para el hito 2). Se alimenta con los trozos crudos
/// que llegan de la red y va entregando eventos completos.
class Parser {
public:
    /// Añade bytes crudos del cuerpo de la respuesta. Los eventos completos
    /// quedan en una cola interna, listos para extraerse con next_event().
    void feed(std::string_view raw);

    /// Extrae el siguiente evento completo en orden de llegada, o nullopt si
    /// aún no hay ninguno. Los eventos se separan por una línea en blanco;
    /// los datos multilínea se unen con '\n'.
    [[nodiscard]] std::optional<std::string> next_event();

    /// Indica si hay datos acumulados que aún no forman un evento completo.
    [[nodiscard]] bool has_partial_data() const;

private:
    std::string buffer_;
    std::deque<std::string> pending_;
};

/// Tipo de evento decodificado.
enum class EventType {
    Delta,  ///< Data payload con contenido para el cliente.
    Done,   ///< Sentinela data: [DONE]: fin del flujo.
    Comment ///< Línea(s) que solo contienen comentarios; se ignora.
};

/// Evento SSE ya decodificado.
struct Event {
    EventType type = EventType::Comment;
    std::string data; ///< Payload unido con '\n' (vacío en Comment y Done).
};

/// Decodifica el payload crudo de un evento: extrae las líneas data:, ignora
/// comentarios y campos no usados (event/id/retry). Devuelve Comment si el
/// evento no tiene datos. BadResponse si no se puede interpretar.
[[nodiscard]] Result<Event> decode_event(const std::string& raw);

/// Interpreta el data payload de un chunk de OpenAI y extrae el incremento
/// de texto (choices[0].delta.content). Nullopt si este chunk no aporta
/// texto (por ejemplo, el último, que cierra con delta vacío y finish_reason).
/// BadResponse si el JSON está roto o la estructura es inesperada.
/// Si finish_reason no es nulo y el chunk trae choices[0].finish_reason como
/// cadena, se escribe ahí; si es null o falta, no se toca.
[[nodiscard]] Result<std::optional<std::string>> decode_openai_chunk(
    const std::string& data, std::string* finish_reason = nullptr);

} // namespace sse
} // namespace chatbot

#endif // CHATBOT_SRC_SSE_H
