#ifndef CHATBOT_CLI_SIDEBAR_H
#define CHATBOT_CLI_SIDEBAR_H

#include "conversation_list.h"
#include "conversation_store.h"

#include <cstddef>
#include <ctime>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace chatbot::cli {

/// Un día del calendario (sin hora).
struct CalendarDay {
    int year = 1970;
    unsigned month = 1;
    unsigned day = 1;

    bool operator==(const CalendarDay&) const = default;
};

/// Día local de un instante (chatbot::local_time); nullopt si no se puede convertir.
[[nodiscard]] std::optional<CalendarDay> local_day(std::time_t time);

/// Día de un updated_at tal como quedó escrito ("2026-10-02T23:58:00-06:00"
/// → 2026-10-02): la fecha local de cuando se guardó, la misma lectura que
/// ConversationList::format_date. nullopt si no es una fecha válida.
[[nodiscard]] std::optional<CalendarDay> day_of(std::string_view iso);

/// Grupo de un día visto desde today: "Hoy", "Ayer", "Últimos 7 días" (de 2
/// a 6 días antes) y si no la fecha corta ("2 oct"; con año si no es el de
/// today: "15 dic 2025"). Un día posterior a today cuenta como "Hoy".
[[nodiscard]] std::string group_label(CalendarDay day, CalendarDay today);

/// Una fila de la barra lateral.
struct SidebarRow {
    enum class Kind {
        New,          ///< "+ Nueva  (Ctrl+N)", siempre la primera.
        Settings,     ///< "⚙ Configuración  (F2)", siempre la segunda.
        Header,       ///< Nombre de un grupo por fecha; no se selecciona.
        Conversation, ///< Una conversación; index es su posición en la lista.
    };
    Kind kind = Kind::New;
    std::string text;      ///< Header: el nombre del grupo.
    std::size_t index = 0; ///< Conversation: índice en list().items().
};

/// Modelo de la barra lateral de conversaciones: filas agrupadas por fecha,
/// selección (las filas "+ Nueva" y "⚙ Configuración" o una conversación),
/// navegación con teclado
/// que salta los encabezados, desplazamiento y clic. Reusa ConversationList
/// para la selección, la confirmación de borrado, can_open e is_current. Sin
/// FTXUI; se usa solo desde el hilo de la interfaz.
class Sidebar {
public:
    /// Grupo de las conversaciones sin fecha válida (por ejemplo, ilegibles).
    static constexpr std::string_view kUndated = "Sin fecha";

    /// Al empezar: elementos (ya ordenados por el almacén) y la conversación
    /// abierta, que queda seleccionada si está guardada; si no, "+ Nueva".
    void open(std::vector<ConversationSummary> items, std::string current_id);

    /// Tras guardar, borrar o abrir: conserva la fila seleccionada si sigue
    /// existiendo (si no, la misma posición) y marca la conversación abierta.
    void refresh(std::vector<ConversationSummary> items, std::string current_id);

    /// Filas para el día today, en el orden de la lista (la más reciente
    /// primero). Los grupos sin conversaciones no aparecen.
    [[nodiscard]] std::vector<SidebarRow> rows(CalendarDay today) const;

    /// Índice en rows de la fila seleccionada.
    [[nodiscard]] std::size_t selected_row(const std::vector<SidebarRow>& rows) const;
    [[nodiscard]] bool new_selected() const { return pick_ == Pick::New; }
    [[nodiscard]] bool settings_selected() const { return pick_ == Pick::Settings; }

    /// Procesa una tecla con la barra enfocada. ↑/↓, PgUp/PgDn y Home/End
    /// se mueven saltando encabezados (Home y End van a la primera y a la
    /// última conversación; sin conversaciones, End va a "⚙ Configuración").
    /// Enter abre (Open), pide una nueva (New) o la configuración
    /// (Settings); Supr pide confirmación y 's' borra (Delete); Esc devuelve
    /// el foco (Close). page_size: filas visibles.
    [[nodiscard]] ListAction handle(ListKey key, std::string_view character, std::size_t page_size);

    /// Primera fila visible.
    [[nodiscard]] std::size_t top() const { return top_; }
    /// Ajusta la vista a view_height filas y la mueve para que la selección
    /// se vea (después de usar el teclado).
    void fit(const std::vector<SidebarRow>& rows, std::size_t view_height);
    /// Rueda: mueve la vista lines filas. Si la selección queda fuera, pasa a
    /// la fila seleccionable visible más cercana.
    void scroll(int lines, const std::vector<SidebarRow>& rows, std::size_t view_height);
    /// Clic en la línea line de la zona visible (0 = la primera visible).
    /// Selecciona la fila y devuelve New, Settings u Open; None si es un encabezado,
    /// una ilegible o no hay fila ahí.
    [[nodiscard]] ListAction click(std::size_t line, const std::vector<SidebarRow>& rows);

    [[nodiscard]] const ConversationList& list() const { return list_; }
    /// Texto de la confirmación de borrado pendiente, si la hay.
    [[nodiscard]] std::optional<std::string> confirmation() const { return list_.confirmation(); }

private:
    /// Selecciona la fila row (New, Settings o Conversation).
    void select_row(const SidebarRow& row);
    /// Va a la conversación index desde una fila fija.
    void pick_conversation(std::size_t index);

    /// Qué fila está seleccionada: una fija o la conversación de list_.
    enum class Pick { New, Settings, Conversation };
    ConversationList list_;
    Pick pick_ = Pick::New;
    std::size_t top_ = 0;
};

} // namespace chatbot::cli

#endif // CHATBOT_CLI_SIDEBAR_H
