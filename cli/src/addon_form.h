#ifndef CHATBOT_CLI_ADDON_FORM_H
#define CHATBOT_CLI_ADDON_FORM_H

#include "service_control.h"

#include "chatbot/addon_settings/telegram_section.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/// F2 → Addons → Telegram, sin FTXUI: los valores del formulario, el error
/// de cada campo (con las reglas de addon_settings, las mismas del bot), la
/// sección y las llaves de credentials.json que se escriben, y los textos del
/// estado del addon y de su servicio.
namespace chatbot::cli {

/// Los campos de texto, en el orden en que se muestran.
enum class AddonField : std::size_t {
    Token,
    Model,
    ModelKey,
    SearchKey,
    AllowedUsers,
    InviteCode,
    DailyLimit,
    Concurrent,
    IdleMinutes,
    StaleMinutes,
    DraftMs,
    TimeZone,
    Count,
};
inline constexpr std::size_t kAddonFieldCount = static_cast<std::size_t>(AddonField::Count);

/// La etiqueta del campo (≤ 20 caracteres: caben a 80 columnas).
[[nodiscard]] std::string_view addon_field_label(AddonField field);

/// Los secretos del bot guardados en credentials.json (vacíos si no hay).
struct AddonSecrets {
    std::string token;
    std::string model_key;
    std::string search_key;
};

/// Lo que edita el formulario. Los campos de los secretos (Token, ModelKey,
/// SearchKey) tienen lo escrito: vacío = se conserva lo guardado.
struct AddonForm {
    bool enabled = false;
    bool log_error_details = false;
    std::array<std::string, kAddonFieldCount> text;
    bool remove_model_key = false;  ///< [Quitar]: se usará la de la app.
    bool remove_search_key = false; ///< [Quitar]: se usará la de la app.

    [[nodiscard]] std::string& at(AddonField field) {
        return text[static_cast<std::size_t>(field)];
    }
    [[nodiscard]] const std::string& at(AddonField field) const {
        return text[static_cast<std::size_t>(field)];
    }
    friend bool operator==(const AddonForm&, const AddonForm&) = default;
};

/// El formulario de una sección leída, con los secretos sin escribir.
[[nodiscard]] AddonForm form_from_section(const addon_settings::TelegramSection& section);

/// El error del campo para mostrar debajo, o nullopt. Nunca repite un
/// secreto ni el código de invitación.
[[nodiscard]] std::optional<std::string> addon_field_error(const AddonForm& form,
                                                           const AddonSecrets& saved,
                                                           AddonField field);

/// El primer campo con error, para la línea de estado al guardar
/// ("<etiqueta>: <error>"), o nullopt.
[[nodiscard]] std::optional<std::string> addon_form_error(const AddonForm& form,
                                                          const AddonSecrets& saved);

/// La sección que se escribe (el formulario ya no tiene errores).
[[nodiscard]] addon_settings::TelegramSection section_from_form(const AddonForm& form);

/// Lo que cambia en credentials.json: llave y valor nuevo, o nullopt para
/// borrarla.
using CredentialChange = std::pair<std::string, std::optional<std::string>>;
[[nodiscard]] std::vector<CredentialChange> addon_credential_changes(const AddonForm& form);

/// Cambió algo que el bot solo toma al arrancar: el token (uno escrito y
/// distinto del guardado) o las respuestas a la vez.
[[nodiscard]] bool addon_needs_restart(const AddonForm& saved_form, const AddonSecrets& saved,
                                       const AddonForm& form);

/// En un campo de secreto vacío (su placeholder), siempre enmascarado:
/// "guardada: …abcd", "sin configurar" o, tras [Quitar], "se quita: se usará
/// la de la app".
[[nodiscard]] std::string addon_secret_status(std::string_view saved, bool removed);

/// La nota de las variables de entorno definidas en la terminal de la app
/// (CHAT_TELEGRAM_TOKEN, CHAT_API_KEY, CHAT_SEARCH_API_KEY), que el servicio
/// no ve; "" si no hay.
[[nodiscard]] std::string addon_env_note(const std::vector<std::string_view>& defined);

/// Lo que se supo del programa del addon (--config-version).
struct AddonInstall {
    std::optional<std::string> path;   ///< nullopt: no está.
    std::optional<int> config_version; ///< nullopt: no contestó un entero.
};

/// "No instalado: copia chatbot-telegram junto a chatbot o en el PATH." (en
/// modo portable, solo "junto a chatbot"), "Incompatible: …" o "" si está
/// instalado y es compatible (la ruta se dibuja aparte).
[[nodiscard]] std::string addon_install_problem(const addon_settings::AddonInfo& addon,
                                                const AddonInstall& install, bool portable);

/// La versión que imprimió --config-version (un entero y salto de línea).
[[nodiscard]] std::optional<int> parse_config_version(const ProcessResult& result);

/// "Servicio: activo · habilitado", "Servicio: no instalado"… (sin systemd:
/// kNoUserSystemd).
[[nodiscard]] std::string service_summary(const ServiceStatus& status);

/// El error del servicio, si hay uno: el mensaje del código de salida
/// (exit_status_message) o, si no tiene, la última línea de error del
/// registro. "" si no hay.
[[nodiscard]] std::string service_problem(const ServiceStatus& status,
                                          std::string_view executable,
                                          std::string_view data_dir);

/// El servicio no está como dice Activado (enabled): "Activado, pero el
/// servicio está detenido: guarda para arrancarlo.", "Desactivado, pero el
/// servicio sigue corriendo: guarda para detenerlo."… "" si coincide (o sin
/// systemd).
[[nodiscard]] std::string service_mismatch(const ServiceStatus& status, bool enabled);

/// El comando para ver el registro.
[[nodiscard]] std::string journal_command(const addon_settings::AddonInfo& addon);

} // namespace chatbot::cli

#endif // CHATBOT_CLI_ADDON_FORM_H
