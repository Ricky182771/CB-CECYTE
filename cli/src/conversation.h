#ifndef CHATBOT_CLI_CONVERSATION_H
#define CHATBOT_CLI_CONVERSATION_H

#include "chatbot/error.h"
#include "chatbot/types.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace chatbot::cli {

/// Prompt de sistema con el que empieza toda conversación. Hacerlo
/// configurable queda fuera del hito 3.
inline constexpr std::string_view kSystemPrompt =
    "Eres un asistente útil. Responde en español, de forma clara y concisa.";

/// Tipo de una entrada de la pantalla.
enum class EntryKind {
    User,
    Assistant,
    Error,
    Notice, ///< Texto informativo, no es un error (p. ej. "Respuesta cancelada.").
};

/// Una entrada de la pantalla. Es independiente del historial para la API:
/// los errores y las respuestas incompletas se muestran, pero no se envían.
struct Entry {
    EntryKind kind = EntryKind::User;
    std::string text;
    bool in_progress = false; ///< Respuesta del asistente que sigue llegando.
    bool incomplete = false;  ///< Respuesta cortada por un error.
    bool cancelled = false;   ///< Respuesta cortada porque el usuario canceló.
    std::string note;         ///< Nota de cómo terminó (finish_reason), o vacía.
};

/// Estado de la conversación. Se usa solo desde el hilo de la interfaz y no
/// sabe nada de FTXUI ni de hilos.
class Conversation {
public:
    /// El historial empieza con el mensaje de sistema (kSystemPrompt).
    Conversation();

    /// Recorta espacios en los extremos. Si el texto queda vacío o hay una
    /// petición en curso, no hace nada y devuelve nullopt. Si no, agrega la
    /// entrada de usuario y el mensaje al historial, marca "ocupado" y
    /// devuelve una copia del historial para enviarla.
    [[nodiscard]] std::optional<std::vector<Message>> submit(std::string_view text);

    /// Agrega texto a la respuesta en curso; la crea con el primer delta.
    /// Sin petición en curso o con texto vacío, no hace nada.
    void append_delta(std::string_view text);

    /// Cierra la respuesta y la agrega al historial. Si no llegó texto, se
    /// trata como error ("El modelo no devolvió texto.") y devuelve el texto
    /// del usuario, igual que finish_error(); si salió bien, nullopt.
    /// finish_reason ("stop", "length", ...) solo agrega una nota a la
    /// entrada: el texto entra al historial porque es texto real.
    [[nodiscard]] std::optional<std::string> finish_success(std::string_view finish_reason = {});

    /// Quita del historial el último mensaje de usuario (no de la pantalla),
    /// marca como incompleta la respuesta parcial si la hubo y agrega una
    /// entrada de error. Devuelve el texto del usuario para que la interfaz
    /// lo regrese a la caja de entrada.
    /// Con ErrorKind::Cancelled no hay entrada de error: la respuesta parcial
    /// se marca cancelada o, si no hubo texto, se agrega un aviso (Notice).
    [[nodiscard]] std::string finish_error(const ChatError& error);

    [[nodiscard]] bool busy() const { return busy_; }
    [[nodiscard]] const std::vector<Entry>& entries() const { return entries_; }
    [[nodiscard]] const std::vector<Message>& history() const { return history_; }

private:
    /// Respuesta del asistente en curso, o nullptr si aún no llega texto.
    Entry* current_answer();

    std::vector<Message> history_; ///< Lo que se envía a la API.
    std::vector<Entry> entries_;   ///< Lo que se muestra en pantalla.
    std::string pending_user_text_;
    bool busy_ = false;
};

} // namespace chatbot::cli

#endif // CHATBOT_CLI_CONVERSATION_H
