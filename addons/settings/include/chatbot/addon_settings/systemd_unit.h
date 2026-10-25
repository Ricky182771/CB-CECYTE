#ifndef CHATBOT_ADDON_SETTINGS_SYSTEMD_UNIT_H
#define CHATBOT_ADDON_SETTINGS_SYSTEMD_UNIT_H

#include <optional>
#include <string>
#include <string_view>

/// La unidad de systemd de usuario de un addon, como la escribe la pantalla
/// de configuración de la app (funciones puras). ExecStart arranca
/// "chatbot <addon>", que pasa el control al addon con exec: systemd y las
/// señales siguen viendo el mismo proceso.
namespace chatbot::addon_settings {

/// Primera línea de toda unidad que escribe la app. Una unidad sin ella es
/// ajena (por ejemplo, hecha a mano con el README): nunca se sobrescribe sin
/// preguntar.
inline constexpr std::string_view kUnitMarker =
    "# Generado por chatbot (Configuración → Addons). No lo edites: se reescribe desde ahí.";

/// Lo que cambia de una instalación a otra.
struct UnitParams {
    std::string chatbot_path; ///< Ruta absoluta de chatbot (executable_path()).
    std::string addon;        ///< "telegram": el argumento de chatbot.
    std::string description;  ///< "Bot de Telegram del chatbot de CECyTE".
    /// Las de la app, solo si son rutas absolutas: el systemd de usuario no
    /// ve las de la shell y, sin ellas, el servicio leería otro config.json.
    std::optional<std::string> xdg_config_home;
    std::optional<std::string> xdg_data_home;
};

/// Un argumento de ExecStart= entre comillas dobles, con las reglas de
/// systemd: \ y " con \, % como %% (especificadores), $ como $$ (variables)
/// y los controles como \xNN.
[[nodiscard]] std::string quote_exec_argument(std::string_view argument);

/// Una asignación de Environment= ("NOMBRE=valor") entre comillas dobles:
/// \ y " con \, % como %% y los controles como \xNN ($ no se expande ahí).
[[nodiscard]] std::string quote_environment(std::string_view name, std::string_view value);

/// El texto completo de la unidad (empieza con kUnitMarker):
/// Type=exec, ExecStart=<chatbot> <addon>, ExecReload=kill -HUP $MAINPID,
/// Restart=on-failure, RestartSec=10, RestartPreventExitStatus=65 69 73 78, un
/// Environment= por cada XDG_* absoluto y WantedBy=default.target.
[[nodiscard]] std::string unit_text(const UnitParams& params);

/// Cómo está la unidad que ya hay en disco frente a la que se escribiría.
enum class UnitState {
    Missing, ///< No hay archivo.
    Foreign, ///< No empieza con kUnitMarker: preguntar antes de reemplazarla.
    Current, ///< Es igual a unit_text(params).
    Stale,   ///< Es de la app pero cambió (se movió chatbot o cambió un XDG_*).
};

/// existing: el contenido del archivo, o nullopt si no existe.
[[nodiscard]] UnitState classify_unit(const std::optional<std::string>& existing,
                                      const UnitParams& params);

} // namespace chatbot::addon_settings

#endif // CHATBOT_ADDON_SETTINGS_SYSTEMD_UNIT_H
