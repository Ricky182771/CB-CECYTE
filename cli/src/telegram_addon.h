#ifndef CHATBOT_CLI_TELEGRAM_ADDON_H
#define CHATBOT_CLI_TELEGRAM_ADDON_H

#include "addon_form.h"
#include "addon_launcher.h"
#include "service_control.h"

#include "chatbot/addon_settings/registry.h"
#include "chatbot/addon_settings/systemd_unit.h"
#include "chatbot/addon_settings/telegram_section.h"
#include "chatbot/platform.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace chatbot::cli {

/// [Reiniciar] (F2) o service.restart sin Activado guardado.
inline constexpr std::string_view kRestartNeedsEnabled =
    "Activa el addon y guarda para arrancarlo.";

/// Lo que el addon necesita del sistema: addons_environment (app_startup.h)
/// lo arma con el real y las pruebas con procesos falsos y una carpeta
/// temporal.
struct AddonsEnvironment {
    Os os = current_os();
    ServiceWorker::Post post;  ///< Pasa una tarea al hilo de la interfaz.
    ProcessRunner run;         ///< run_process.
    AddonLocator locate;       ///< find_addon_executable junto a chatbot_path.
    bool portable = false;
    std::string config_path;
    std::string credentials_path;
    std::optional<std::string> data_dir;  ///< default_addon_data_dir; nullopt: sin HOME.
    std::optional<std::string> unit_path; ///< default_unit_path; nullopt: sin HOME.
    std::optional<unsigned long> uid;     ///< current_user_id, para loginctl.
    /// La ruta de chatbot (la TUI, que hace exec del addon): el ExecStart de
    /// la unidad. En chatbot-rpc, el chatbot de su misma carpeta.
    std::string chatbot_path;
    std::optional<std::string> xdg_config_home; ///< Solo si es ruta absoluta.
    std::optional<std::string> xdg_data_home;   ///< Solo si es ruta absoluta.
    /// Los secretos definidos en el entorno de la app (secrets.h), que el
    /// servicio no ve.
    std::vector<std::string_view> env_defined;
    std::string home; ///< Para mostrar las rutas con ~.
};

/// F2 → Addons → Telegram sin FTXUI, lo que comparten AddonsPanel y
/// chatbot-rpc: lo guardado (la sección, los secretos y los invitados),
/// revisar y guardar un formulario, y el addon y su servicio, que corren en
/// un ServiceWorker. No pregunta nada: dice qué hay que preguntar
/// (discard_question, needs_restart, ServiceAction::Kind::Confirm) y quien
/// llama pregunta y vuelve a llamar. Se usa solo desde el hilo de la
/// interfaz; los done corren ahí, con el estado ya al día.
class TelegramAddon {
public:
    /// En Windows, sin hilo; sin unit_path (sin HOME), sin servicio.
    explicit TelegramAddon(AddonsEnvironment environment);
    ~TelegramAddon();
    TelegramAddon(const TelegramAddon&) = delete;
    TelegramAddon& operator=(const TelegramAddon&) = delete;
    TelegramAddon(TelegramAddon&&) = delete;
    TelegramAddon& operator=(TelegramAddon&&) = delete;

    [[nodiscard]] const addon_settings::AddonInfo& info() const { return addon_; }
    [[nodiscard]] const AddonsEnvironment& environment() const { return environment_; }
    /// No es Windows.
    [[nodiscard]] bool available() const { return environment_.os != Os::Windows; }
    /// Hay dónde instalar el servicio (HOME): los métodos de servicio sirven.
    [[nodiscard]] bool has_service() const { return control_ != nullptr; }
    /// Los parámetros de la unidad (ExecStart = chatbot_path).
    [[nodiscard]] addon_settings::UnitParams unit_params() const;

    /// Lee la sección, los secretos y los invitados. El estado del addon y
    /// del servicio se queda (lo trae refresh).
    void load();
    /// Olvida el estado del addon y del servicio (F2, al abrir).
    void forget_status();
    [[nodiscard]] const AddonForm& saved_form() const { return saved_form_; }
    [[nodiscard]] const AddonSecrets& secrets() const { return secrets_; }
    /// config.json o credentials.json no se pudieron leer ("" si sí).
    [[nodiscard]] const std::string& load_error() const { return load_error_; }
    /// La sección es de una versión más nueva de la configuración.
    [[nodiscard]] bool newer() const { return read_.newer(); }
    /// Error al leer, sección más nueva o addon incompatible: no se guarda.
    [[nodiscard]] bool read_only() const;
    /// Por qué la sección es de solo lectura: el error al leer ("… Solo
    /// lectura.") o la versión más nueva; "" si no (el addon incompatible
    /// se dice con addon_install_problem).
    [[nodiscard]] std::string section_problem() const;
    /// Las llaves de la sección que guardar descartaría.
    [[nodiscard]] const std::vector<std::string>& discarded() const { return read_.discarded; }
    /// "Se descartarán: a, b. ¿Guardar? (s/n)", o "" si no hay.
    [[nodiscard]] std::string discard_question() const;
    [[nodiscard]] const std::vector<std::int64_t>& invited() const { return invited_; }
    /// invitados.json no se pudo leer (o falta HOME): "" si se pudo.
    [[nodiscard]] const std::string& invited_error() const { return invited_error_; }
    /// nullopt: aún no se sabe (check_install, refresh).
    [[nodiscard]] const std::optional<AddonInstall>& install() const { return install_; }
    /// nullopt: aún no se sabe (refresh y las acciones).
    [[nodiscard]] const std::optional<ServiceStatus>& service() const { return service_; }

    /// Guardar el formulario escribiría algo.
    [[nodiscard]] bool changed(const AddonForm& form) const { return form != saved_form_; }
    /// El aviso de service_mismatch con enabled, si el addon está instalado,
    /// es compatible y se sabe el estado; "" si no o si coincide.
    [[nodiscard]] std::string out_of_sync(bool enabled) const;
    /// Guardar form necesitaría reiniciar el bot: addon_needs_restart, form
    /// activado y el servicio activo según el último estado.
    [[nodiscard]] bool needs_restart(const AddonForm& form) const;
    /// Escribe primero los cambios de credentials.json y después la sección
    /// (si fallan los secretos, la sección no se escribe), y lo escrito pasa a
    /// ser lo guardado. Quien llama ya revisó read_only, addon_form_error y
    /// discard_question. El error, o nullopt.
    [[nodiscard]] std::optional<std::string> save(const AddonForm& form);

    /// Un código de invitación nuevo (no guarda nada).
    [[nodiscard]] static Result<std::string> invite_code();

    /// Lo que pasó al quitar un invitado.
    struct Removal {
        std::optional<std::string> error; ///< No se pudo: invitados.json o sin HOME.
        std::string message;              ///< "Se quitó …" y, si recargó, el resultado.
    };
    /// Quita user de invitados.json al momento y, si el servicio está activo
    /// (según el último estado), lo recarga en el hilo. done puede correr
    /// antes de regresar (sin recarga).
    void remove_invited(std::int64_t user, std::function<void(const Removal&)> done);

    /// --config-version, en el hilo. Requiere available.
    void check_install(std::function<void()> done);
    /// --config-version y el estado del servicio. Requiere has_service.
    void refresh(std::function<void()> done);

    /// Lo que pasó al aplicar el servicio.
    struct Applied {
        ServiceAction action;
        /// Activado y el addon no está instalado: el problema, y no se tocó
        /// el servicio.
        std::string skipped;
    };
    /// Lleva el servicio a lo que dice Activado en lo guardado
    /// (ServiceControl::apply) y trae su estado. Requiere has_service.
    void apply(bool restart, bool replace_foreign, std::function<void(const Applied&)> done);
    /// restart, disable y reload_if_active; restart y disable traen también
    /// el estado. Requieren has_service.
    void restart(std::function<void(const ServiceAction&)> done);
    void disable(std::function<void(const ServiceAction&)> done);
    void reload(std::function<void(const ServiceAction&)> done);

private:
    [[nodiscard]] AddonInstall find_install(const CancelToken& cancel) const;

    const addon_settings::AddonInfo& addon_;
    AddonsEnvironment environment_;
    addon_settings::TelegramSectionRead read_;
    std::string load_error_;
    AddonSecrets secrets_;
    AddonForm saved_form_; ///< Lo de config.json (al leer o al guardar).
    std::optional<AddonInstall> install_;
    std::optional<ServiceStatus> service_;
    std::vector<std::int64_t> invited_;
    std::string invited_error_;

    // control_ antes que worker_: el destructor del worker espera al trabajo
    // en curso, que usa control_.
    std::unique_ptr<ServiceControl> control_;
    std::unique_ptr<ServiceWorker> worker_;
};

} // namespace chatbot::cli

#endif // CHATBOT_CLI_TELEGRAM_ADDON_H
