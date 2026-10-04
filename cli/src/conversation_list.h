#ifndef CHATBOT_CLI_CONVERSATION_LIST_H
#define CHATBOT_CLI_CONVERSATION_LIST_H

#include "conversation_store.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace chatbot::cli {

/// Teclas que le importan a la lista (main.cpp traduce las de FTXUI).
enum class ListKey { Up, Down, PageUp, PageDown, Home, End, Enter, Escape, Delete, Other };

/// Lo que la interfaz debe hacer después de una tecla.
struct ListAction {
    enum class Type { None, Open, Close, Delete, New }; ///< New: conversación nueva (barra lateral).
    Type type = Type::None;
    std::string id; ///< Para Open y Delete.
};

/// Estado de la lista de conversaciones: selección, qué se puede abrir y la
/// confirmación de borrado. Sin FTXUI; se usa solo desde el hilo de la
/// interfaz.
class ConversationList {
public:
    static constexpr std::string_view kEmptyMessage = "No hay conversaciones guardadas.";

    /// Al abrir la lista: elementos (ya ordenados por el almacén) y la
    /// conversación actual, que queda seleccionada si está.
    void open(std::vector<ConversationSummary> items, std::string current_id);

    /// Tras un cambio (p. ej. un borrado): conserva el elemento seleccionado
    /// o, si ya no está, la misma posición.
    void refresh(std::vector<ConversationSummary> items);

    /// Selecciona el elemento index (si existe) y cancela una confirmación
    /// de borrado pendiente. Para la barra lateral (clic, rueda).
    void select(std::size_t index);

    /// Cambia la conversación abierta (la marcada con is_current).
    void set_current(std::string current_id);

    /// Procesa una tecla. character es el texto de la tecla si es un
    /// carácter. page_size es cuántas filas se ven (para PgUp/PgDn).
    [[nodiscard]] ListAction handle(ListKey key, std::string_view character, std::size_t page_size);

    [[nodiscard]] const std::vector<ConversationSummary>& items() const { return items_; }
    [[nodiscard]] bool empty() const { return items_.empty(); }
    [[nodiscard]] std::size_t selected() const { return selected_; }
    [[nodiscard]] bool is_current(std::size_t index) const;
    /// Las ilegibles no se pueden abrir ni borrar.
    [[nodiscard]] bool can_open(std::size_t index) const;
    /// Texto de la confirmación de borrado pendiente, si la hay.
    [[nodiscard]] std::optional<std::string> confirmation() const;

    /// "2026-10-02T23:58:00-06:00" → "2026-10-02 23:58" (si no, tal cual).
    [[nodiscard]] static std::string format_date(const std::string& iso);
    /// Fecha, número de mensajes y modelo; o el motivo si es ilegible.
    [[nodiscard]] static std::string details(const ConversationSummary& summary);

private:
    std::vector<ConversationSummary> items_;
    std::string current_id_;
    std::size_t selected_ = 0;
    bool confirming_delete_ = false;
};

} // namespace chatbot::cli

#endif // CHATBOT_CLI_CONVERSATION_LIST_H
