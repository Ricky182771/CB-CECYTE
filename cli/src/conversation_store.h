#ifndef CHATBOT_CLI_CONVERSATION_STORE_H
#define CHATBOT_CLI_CONVERSATION_STORE_H

#include "chatbot/types.h"
#include "chatbot/web_search.h"

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace chatbot::cli {

/// Versión del esquema de los archivos de conversación.
inline constexpr int kConversationFormatVersion = 1;

/// Largo máximo del título, en puntos de código (sin contar el "…").
inline constexpr std::size_t kTitleMaxCodePoints = 60;

/// Búsqueda web de un mensaje del usuario (/buscar), tal como se guarda:
/// con ella se reconstruye el bloque que se envía y las fuentes.
struct StoredSearch {
    std::string date;        ///< "AAAA-MM-DD" local del día de la búsqueda.
    SearchResponse response; ///< Consulta y resultados ya recortados (trim_search_response).
};

/// Un mensaje guardado. El de sistema nunca se guarda.
struct StoredMessage {
    Role role = Role::User;
    std::string content;       ///< En un /buscar, el texto que escribió el usuario.
    std::string model;         ///< Solo en respuestas del asistente.
    std::string finish_reason; ///< Solo en respuestas del asistente.
    std::optional<StoredSearch> search; ///< Solo en mensajes del usuario hechos con /buscar.
};

/// Una conversación en su forma guardable (esquema versión 1; "search" es
/// una llave opcional que las versiones anteriores ignoran).
struct StoredConversation {
    std::string id;
    std::string title;
    std::string created_at; ///< ISO 8601 con desfase.
    std::string updated_at; ///< ISO 8601 con desfase.
    /// Pares usuario → asistente alternados, empezando en User y terminando
    /// en Assistant (al menos uno). Al leer, un archivo que no cumple es
    /// ilegible.
    std::vector<StoredMessage> messages;
};

/// Resumen de un archivo para la lista de conversaciones.
struct ConversationSummary {
    std::string id;
    std::string title;
    std::string updated_at;
    std::size_t message_count = 0;
    std::string last_model;
    bool readable = true;
    std::string error; ///< Motivo, si no se pudo leer.
};

/// Resultado de cargar: la conversación o un mensaje de error en español.
struct LoadResult {
    std::optional<StoredConversation> conversation;
    std::string error;
};

/// Directorio de las conversaciones (función pura, para poder probarla):
/// - chat_data_dir (CHAT_DATA_DIR) si está y no está vacío;
/// - si no, $XDG_DATA_HOME/chatbot/conversations, solo si es ruta absoluta
///   (la especificación XDG manda ignorar rutas relativas);
/// - si no, $HOME/.local/share/chatbot/conversations;
/// - nullopt si no hay nada.
[[nodiscard]] std::optional<std::string> resolve_data_dir(
    const std::optional<std::string>& chat_data_dir,
    const std::optional<std::string>& xdg_data_home,
    const std::optional<std::string>& home);

/// Título a partir del primer mensaje del usuario: espacios y saltos de línea
/// colapsados a un espacio, truncado a kTitleMaxCodePoints puntos de código
/// sin partir un carácter UTF-8, más "…" si se truncó.
[[nodiscard]] std::string make_title(std::string_view first_user_message);

/// Fecha ISO 8601 local con desfase ("2026-10-02T23:58:00-06:00"), con
/// chatbot::local_time + strftime ("%z" da "-0600" en POSIX y en UCRT).
[[nodiscard]] std::string format_iso8601(std::time_t time);

/// id = AAAAMMDD-HHMMSS-xxxxxx (hora local + 6 hex).
[[nodiscard]] std::string format_conversation_id(std::time_t time, std::uint32_t random);

/// Lee y escribe conversaciones, un archivo <id>.json por conversación.
/// Ninguna operación lanza excepciones. Se usa desde un solo hilo.
class ConversationStore {
public:
    /// dir vacío: todas las operaciones fallan con un mensaje claro.
    explicit ConversationStore(std::string dir);

    [[nodiscard]] const std::string& dir() const { return dir_; }

    /// Un id nuevo que no existe todavía en el directorio.
    [[nodiscard]] std::string new_id() const;

    /// Un resumen por cada *.json, el más reciente primero. Los ilegibles
    /// (corruptos, de una versión desconocida o con mensajes que no alternan)
    /// aparecen marcados, al final.
    [[nodiscard]] std::vector<ConversationSummary> list() const;

    [[nodiscard]] LoadResult load(const std::string& id) const;

    /// Escritura atómica con chatbot::write_file_atomic: <id>.json.tmp (0600)
    /// y reemplazo. Crea el directorio con 0700 si no existe; si ya existía,
    /// no toca sus permisos. Nunca sobrescribe un archivo ilegible. Devuelve
    /// el error o nullopt.
    [[nodiscard]] std::optional<std::string> save(const StoredConversation& conversation) const;

    /// Borra la conversación. Nunca borra un archivo ilegible.
    [[nodiscard]] std::optional<std::string> remove(const std::string& id) const;

private:
    [[nodiscard]] std::string path_for(const std::string& id) const;

    std::string dir_;
};

} // namespace chatbot::cli

#endif // CHATBOT_CLI_CONVERSATION_STORE_H
