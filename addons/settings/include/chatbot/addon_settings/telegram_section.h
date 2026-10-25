#ifndef CHATBOT_ADDON_SETTINGS_TELEGRAM_SECTION_H
#define CHATBOT_ADDON_SETTINGS_TELEGRAM_SECTION_H

#include "chatbot/error.h"
#include "chatbot/result.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/// La sección "addons" → "telegram" de config.json: la escribe la pantalla de
/// configuración de la app (F2 → Addons) y la lee chatbot-telegram. El
/// proveedor, la URL base, las instrucciones del sistema, history_limit y
/// timeout_seconds no van aquí: salen del resto de config.json, como en la
/// app. Los secretos tampoco (secrets.h).
namespace chatbot::addon_settings {

/// Versión del esquema de la sección que entiende este código. El bot no
/// arranca con otra; F2 muestra una mayor en solo lectura.
inline constexpr int kTelegramConfigVersion = 1;

/// La llave de config.json donde viven las secciones de los addons.
inline constexpr std::string_view kAddonsKey = "addons";

/// Los valores de la sección, sin revisar los rangos (limits.h). Los
/// predeterminados son los de una sección que no existe: el bot apagado.
struct TelegramSection {
    bool enabled = false;
    std::string model; ///< Vacío: el modelo de la app.
    std::vector<std::int64_t> allowed_users;
    std::string invite_code; ///< Vacío: sin código.
    std::int64_t daily_message_limit = 50;
    std::int64_t max_concurrent_responses = 4;
    std::int64_t session_idle_minutes = 60;
    std::int64_t stale_message_minutes = 10;
    std::int64_t draft_interval_ms = 1000;
    /// Vacío: default_time_zone() (lo resuelve quien la lee).
    std::string time_zone;
    bool log_error_details = false;

    friend bool operator==(const TelegramSection&, const TelegramSection&) = default;
};

/// Lo que se leyó de la sección.
struct TelegramSectionRead {
    TelegramSection values; ///< Lo que se pudo leer; lo demás, por defecto.
    bool present = false;   ///< config.json tiene "addons" → "telegram".
    /// La "version" que traía, si era un entero.
    std::optional<std::int64_t> version;
    /// Las llaves que no se entendieron (desconocidas o de otro tipo), en el
    /// orden del archivo; "version" si falta o no es un entero válido. Con
    /// "addons" o "telegram" que no son objetos, ["addons"] o ["telegram"].
    /// F2 avisa "Se descartarán: …" antes de reescribir la sección.
    std::vector<std::string> discarded;

    /// version es mayor que kTelegramConfigVersion: F2 la muestra en solo
    /// lectura y el bot no arranca.
    [[nodiscard]] bool newer() const {
        return version.has_value() && *version > kTelegramConfigVersion;
    }
    /// Por qué el bot no puede usar la sección (para su error 78), o nullopt:
    /// sin sección, o una sección completa de la versión que entiende.
    [[nodiscard]] std::optional<std::string> problem() const;
};

/// Lee la sección de config.json. Que el archivo o la sección no existan no
/// es un error (present = false). Si config.json no es un objeto JSON
/// válido, error Config.
[[nodiscard]] Result<TelegramSectionRead> read_telegram_section(const std::string& config_path);

/// Escribe la sección completa, con "version": kTelegramConfigVersion y las
/// llaves en el orden del esquema ("model" solo si no está vacío), de forma
/// atómica (save_config_section), conservando las demás llaves de
/// config.json y los otros addons de "addons".
[[nodiscard]] std::optional<ChatError> write_telegram_section(const std::string& config_path,
                                                              const TelegramSection& section);

} // namespace chatbot::addon_settings

#endif // CHATBOT_ADDON_SETTINGS_TELEGRAM_SECTION_H
