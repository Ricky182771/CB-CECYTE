#ifndef CHATBOT_BOT_COMMANDS_H
#define CHATBOT_BOT_COMMANDS_H

#include <string>
#include <string_view>

namespace chatbot::bot {

inline constexpr std::string_view kStartCommand = "/start";
inline constexpr std::string_view kHelpCommand = "/ayuda";
inline constexpr std::string_view kNewCommand = "/nuevo";
inline constexpr std::string_view kSearchCommand = "/buscar";

/// Un mensaje entrante ya interpretado.
struct BotCommand {
    enum class Type {
        Normal,      ///< Texto para el modelo (también un comando desconocido).
        Start,       ///< /start [código]
        Help,        ///< /ayuda
        New,         ///< /nuevo
        Search,      ///< /buscar <consulta>
        SearchEmpty, ///< /buscar sin consulta
    };
    Type type = Type::Normal;
    /// Normal: el texto recortado. Start: el código (vacío si no hay).
    /// Search: la consulta en una línea. Los demás: vacío.
    std::string argument;
};

/// Interpreta el texto como la app: espacios recortados en los extremos;
/// el nombre del comando es exacto (en minúsculas) y va solo o seguido de
/// un espacio o salto de línea ("/buscarx" es texto normal). En los grupos
/// de Telegram el comando llega como "/start@MiBot": se quita lo que va
/// desde la '@'. /ayuda y /nuevo ignoran lo que siga. Un comando
/// desconocido es texto normal y llega al modelo tal cual.
[[nodiscard]] BotCommand parse_bot_command(std::string_view text);

} // namespace chatbot::bot

#endif // CHATBOT_BOT_COMMANDS_H
