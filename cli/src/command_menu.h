#ifndef CHATBOT_CLI_COMMAND_MENU_H
#define CHATBOT_CLI_COMMAND_MENU_H

#include "commands.h"
#include "conversation_list.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace chatbot::cli {

/// Lo que la interfaz debe hacer después de una tecla o un clic en el menú.
struct CommandMenuAction {
    enum class Type {
        Pass,     ///< El menú no la usó: sigue el camino de siempre.
        Handled,  ///< El menú la usó (cursor o cierre); nada más que hacer.
        Complete, ///< Poner text en la caja, con el cursor al final.
        Submit,   ///< Poner text en la caja y enviarlo.
    };
    Type type = Type::Pass;
    std::string text; ///< Para Complete y Submit.
};

/// Estado del menú de comandos que aparece al escribir "/" en la caja:
/// cuándo se ve, el filtro y el cursor. Sin FTXUI; se usa solo desde el
/// hilo de la interfaz.
class CommandMenu {
public:
    /// La fila que se ve cuando ningún comando coincide (sin cursor).
    static constexpr std::string_view kNoMatches =
        "Sin comandos que coincidan · se enviará como mensaje";

    /// Con el texto de la caja y si la caja tiene el foco; se llama antes de
    /// cada tecla y al dibujar. El menú se ve si la caja tiene el foco, el
    /// texto empieza con "/" y no tiene espacios ni saltos de línea (se está
    /// escribiendo el nombre), salvo que se haya cerrado (Esc o al elegir)
    /// con este mismo texto.
    void update(std::string_view text, bool input_focused);

    [[nodiscard]] bool is_open() const { return open_; }
    /// Índices en kCommands de los comandos que coinciden, en orden. Vacío
    /// con el menú abierto: se ve la fila kNoMatches.
    [[nodiscard]] const std::vector<std::size_t>& matches() const { return matches_; }
    /// Fila del cursor dentro de matches() (0 si no hay ninguna).
    [[nodiscard]] std::size_t cursor() const { return cursor_; }

    /// Mueve el cursor a la fila row (si existe). Para el ratón (hover).
    void select(std::size_t row);
    /// Clic en la fila row: lo mismo que Tab con el cursor en ella.
    [[nodiscard]] CommandMenuAction choose(std::size_t row);

    /// Procesa una tecla con el menú abierto (cerrado: Pass).
    /// - ↑/↓ mueven el cursor y dan la vuelta; Home/End y PgUp/PgDn van a
    ///   la primera o a la última fila.
    /// - Tab completa: el nombre más un espacio si el comando lleva
    ///   argumentos, o solo el nombre; y cierra el menú.
    /// - Enter: si el comando necesita argumento, igual que Tab; si no,
    ///   Submit con el nombre.
    /// - Esc cierra el menú (nada más).
    /// Sin coincidencias, todo menos Esc es Pass.
    [[nodiscard]] CommandMenuAction handle(ListKey key);

private:
    /// Completa el comando de la fila row y cierra el menú.
    CommandMenuAction complete(std::size_t row, bool submit_if_possible);
    /// Cierra el menú hasta que el texto de la caja sea distinto de text.
    void dismiss(std::string text);

    bool open_ = false;
    std::string text_; ///< Texto de la caja en el último update.
    std::optional<std::string> dismissed_;
    std::vector<std::size_t> matches_;
    std::size_t cursor_ = 0;
};

} // namespace chatbot::cli

#endif // CHATBOT_CLI_COMMAND_MENU_H
