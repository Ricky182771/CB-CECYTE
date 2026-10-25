#include "service_control.h"

#include "chatbot/addon_settings/paths.h"
#include "chatbot/platform.h"

#include <cctype>
#include <charconv>
#include <exception>
#include <fstream>
#include <iterator>
#include <utility>

namespace chatbot::cli {
namespace {

std::string_view trim(std::string_view text) {
    const std::size_t begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string_view::npos) {
        return {};
    }
    return text.substr(begin, text.find_last_not_of(" \t\r\n") - begin + 1);
}

bool contains_ignoring_case(std::string_view text, std::string_view word) {
    for (std::size_t i = 0; i + word.size() <= text.size(); ++i) {
        std::size_t j = 0;
        while (j < word.size() &&
               std::tolower(static_cast<unsigned char>(text[i + j])) ==
                   std::tolower(static_cast<unsigned char>(word[j]))) {
            ++j;
        }
        if (j == word.size()) {
            return true;
        }
    }
    return false;
}

/// "AAAA-MM-DD HH:MM:SS " al principio de una línea de StreamLog.
bool has_log_stamp(std::string_view line) {
    constexpr std::string_view kShape = "0000-00-00 00:00:00 ";
    if (line.size() < kShape.size()) {
        return false;
    }
    for (std::size_t i = 0; i < kShape.size(); ++i) {
        const bool digit = std::isdigit(static_cast<unsigned char>(line[i])) != 0;
        if (kShape[i] == '0' ? !digit : line[i] != kShape[i]) {
            return false;
        }
    }
    return true;
}

/// Por qué falló un programa, en una línea corta.
std::string failure_detail(const ProcessResult& result) {
    switch (result.status) {
    case ProcessResult::Status::TimedOut:
        return "no respondió en " +
               std::to_string(kServiceCommandTimeout.count() / 1000) + " s";
    case ProcessResult::Status::Cancelled:
        return "se canceló";
    case ProcessResult::Status::Signaled:
        return "terminó por una señal";
    case ProcessResult::Status::NotFound:
    case ProcessResult::Status::Failed:
        return "no se pudo ejecutar";
    case ProcessResult::Status::Exited:
        break;
    }
    std::string_view first = trim(result.err);
    first = first.substr(0, first.find('\n'));
    if (first.empty()) {
        return "salió con " + std::to_string(result.exit_code);
    }
    constexpr std::size_t kMaxDetail = 200;
    std::string detail{first.substr(0, kMaxDetail)};
    while (!detail.empty() && (static_cast<unsigned char>(detail.back()) & 0xC0) == 0x80) {
        detail.pop_back(); // No partir un carácter.
    }
    return detail;
}

bool cancelled(const CancelToken* cancel) {
    return cancel != nullptr && cancel->is_cancelled();
}

std::optional<std::string> read_file(const std::string& path) {
    try {
        std::ifstream file{path, std::ios::binary};
        if (!file) {
            return std::nullopt;
        }
        return std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

} // namespace

std::string exit_status_message(int status, std::string_view executable,
                                std::string_view data_dir) {
    switch (status) {
    case 65:
        return "invitados.json está dañado: " + addon_settings::invited_file_path(data_dir) +
               ". Arréglalo o quítalo de ahí.";
    case 69:
        return "No se encontró " + std::string{executable};
    case 73:
        return "No se puede escribir en la carpeta de datos del bot: " + std::string{data_dir} +
               ".";
    case 78:
        return "Error de configuración: revisa los campos de arriba";
    default:
        return {};
    }
}

std::optional<std::string> show_property(std::string_view show, std::string_view property) {
    while (!show.empty()) {
        const std::size_t end = show.find('\n');
        const std::string_view line = show.substr(0, end);
        if (line.size() > property.size() && line.substr(0, property.size()) == property &&
            line[property.size()] == '=') {
            return std::string{trim(line.substr(property.size() + 1))};
        }
        if (end == std::string_view::npos) {
            break;
        }
        show.remove_prefix(end + 1);
    }
    return std::nullopt;
}

std::string last_error_line(std::string_view journal, std::string_view executable) {
    const std::string fatal = std::string{executable} + ": ";
    std::string last;
    while (!journal.empty()) {
        const std::size_t end = journal.find('\n');
        std::string_view line = journal.substr(0, end);
        bool error = false;
        if (has_log_stamp(line) && line.substr(20, 6) == "error ") {
            line.remove_prefix(26);
            error = true;
        } else if (line.substr(0, fatal.size()) == fatal) {
            line.remove_prefix(fatal.size());
            error = true;
        }
        if (error) {
            // El detalle puede repetir lo que escribió un alumno.
            const std::size_t cut =
                line.substr(0, 8) == "detalle=" ? 0 : line.find(" detalle=");
            line = trim(line.substr(0, cut));
            if (!line.empty()) {
                last = std::string{line};
            }
        }
        if (end == std::string_view::npos) {
            break;
        }
        journal.remove_prefix(end + 1);
    }
    return last;
}

bool service_running(std::string_view active) {
    return !active.empty() && active != "inactive" && active != "failed";
}

bool no_user_systemd(const ProcessResult& result) {
    if (result.status == ProcessResult::Status::NotFound ||
        result.status == ProcessResult::Status::Failed) {
        return true;
    }
    // "Failed to connect to bus" / "…user scope bus…" (sin sesión).
    return trim(result.out).empty() && contains_ignoring_case(result.err, "bus");
}

ServiceControl::ServiceControl(const addon_settings::AddonInfo& addon, std::string unit_path,
                               std::optional<unsigned long> uid, ProcessRunner run)
    : addon_(addon), unit_path_(std::move(unit_path)), uid_(uid), run_(std::move(run)) {}

ProcessResult ServiceControl::run(std::vector<std::string> argv,
                                  const CancelToken* cancel) const {
    ProcessOptions options;
    options.timeout = kServiceCommandTimeout;
    options.cancel = cancel;
    return run_(argv, options);
}

ProcessResult ServiceControl::systemctl(std::vector<std::string> arguments,
                                        const CancelToken* cancel) const {
    arguments.insert(arguments.begin(), {"systemctl", "--user"});
    return run(std::move(arguments), cancel);
}

ServiceStatus ServiceControl::status(const addon_settings::UnitParams& params,
                                     const CancelToken* cancel) const {
    // Cancelada (se cierra la app), no sigue con los programas que faltan.
    ServiceStatus status;
    const std::string unit{addon_.unit};
    const ProcessResult active = systemctl({"is-active", unit}, cancel);
    if (cancelled(cancel)) {
        return status;
    }
    if (no_user_systemd(active)) {
        status.systemd = false;
        return status;
    }
    status.active = std::string{trim(active.out)};
    status.enabled = std::string{trim(systemctl({"is-enabled", unit}, cancel).out)};
    if (cancelled(cancel)) {
        return status;
    }
    const ProcessResult show = systemctl({"show", "-p", "ExecMainStatus,Result", unit}, cancel);
    if (cancelled(cancel)) {
        return status;
    }
    if (const std::optional<std::string> code = show_property(show.out, "ExecMainStatus")) {
        int value = 0;
        const auto [end, error] = std::from_chars(code->data(), code->data() + code->size(), value);
        if (error == std::errc{} && end == code->data() + code->size() && value != 0) {
            status.exit_status = value;
        }
    }
    status.unit = addon_settings::classify_unit(read_file(unit_path_), params);

    if (uid_.has_value()) {
        // El UID numérico: $USER puede no estar o no coincidir.
        const ProcessResult linger = run(
            {"loginctl", "show-user", std::to_string(*uid_), "-p", "Linger", "--value"}, cancel);
        if (cancelled(cancel)) {
            return status;
        }
        const std::string_view value = trim(linger.out);
        if (linger.ok() && (value == "yes" || value == "no")) {
            status.linger = value == "yes";
        }
    }
    const ProcessResult journal =
        run({"journalctl", "--user", "-u", unit, "-o", "cat", "-n", "200", "--no-pager"},
            cancel);
    status.last_error = last_error_line(journal.out, addon_.executable);
    return status;
}

ServiceAction ServiceControl::simple(std::vector<std::string> arguments, std::string_view done,
                                     std::string_view failed, const CancelToken* cancel) const {
    const ProcessResult result = systemctl(std::move(arguments), cancel);
    if (result.ok()) {
        return {ServiceAction::Kind::Done, std::string{done}};
    }
    if (no_user_systemd(result)) {
        return {ServiceAction::Kind::NoSystemd, std::string{kNoUserSystemd}};
    }
    return {ServiceAction::Kind::Failed, std::string{failed} + ": " + failure_detail(result)};
}

ServiceAction ServiceControl::apply(const addon_settings::UnitParams& params, bool enabled,
                                    bool replace_foreign, bool restart_bot,
                                    const CancelToken* cancel) const {
    const std::string unit{addon_.unit};
    const ProcessResult active = systemctl({"is-active", unit}, cancel);
    if (no_user_systemd(active)) {
        return {ServiceAction::Kind::NoSystemd, std::string{kNoUserSystemd}};
    }
    const bool running = service_running(trim(active.out));
    const bool is_enabled = trim(systemctl({"is-enabled", unit}, cancel).out) == "enabled";
    if (!enabled) {
        if (!running && !is_enabled) {
            return {ServiceAction::Kind::Done, ""}; // Ya está como dice la casilla.
        }
        return disable(cancel);
    }
    const addon_settings::UnitState state =
        addon_settings::classify_unit(read_file(unit_path_), params);
    if (state == addon_settings::UnitState::Foreign && !replace_foreign) {
        return {ServiceAction::Kind::Confirm, std::string{kReplaceForeignUnit}};
    }
    if (state != addon_settings::UnitState::Current) {
        if (const std::optional<std::string> error =
                write_file_atomic(unit_path_, addon_settings::unit_text(params),
                                  FilePrivacy::KeepExisting, FolderPrivacy::PrivateIfNew)) {
            return {ServiceAction::Kind::Failed, "No se pudo escribir el servicio: " + *error};
        }
        const ServiceAction reloaded =
            simple({"daemon-reload"}, "", "No se pudo recargar systemd", cancel);
        if (reloaded.kind != ServiceAction::Kind::Done) {
            return reloaded;
        }
    }
    if (!running || !is_enabled) {
        const ServiceAction started = simple({"enable", "--now", unit}, "Servicio activado.",
                                             "No se pudo activar el servicio", cancel);
        if (started.kind != ServiceAction::Kind::Done || !running) {
            return started; // El detenido arranca ya con la configuración nueva.
        }
    }
    if (restart_bot) {
        return restart(cancel);
    }
    return simple({"reload", unit}, "Configuración aplicada al bot.",
                  "No se pudo recargar el bot", cancel);
}

ServiceAction ServiceControl::disable(const CancelToken* cancel) const {
    return simple({"disable", "--now", std::string{addon_.unit}}, "Servicio desactivado.",
                  "No se pudo desactivar el servicio", cancel);
}

ServiceAction ServiceControl::restart(const CancelToken* cancel) const {
    return simple({"restart", std::string{addon_.unit}}, "Bot reiniciado.",
                  "No se pudo reiniciar el bot", cancel);
}

ServiceAction ServiceControl::reload_if_active(const CancelToken* cancel) const {
    const ProcessResult active = systemctl({"is-active", std::string{addon_.unit}}, cancel);
    if (no_user_systemd(active)) {
        return {ServiceAction::Kind::NoSystemd, std::string{kNoUserSystemd}};
    }
    if (trim(active.out) != "active") {
        return {ServiceAction::Kind::Done, ""}; // No corre: lo leerá al arrancar.
    }
    return simple({"reload", std::string{addon_.unit}}, "Configuración aplicada al bot.",
                  "No se pudo recargar el bot", cancel);
}

ServiceWorker::ServiceWorker(Post post)
    : post_(std::move(post)), state_(std::make_shared<State>()), thread_([this] { loop(); }) {}

ServiceWorker::~ServiceWorker() {
    state_->alive = false;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        jobs_.clear();
    }
    cancel_.cancel(); // Mata el programa en curso: no se espera su tope.
    ready_.notify_all();
    thread_.join();
}

void ServiceWorker::push(Job job) {
    ++state_->pending;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        jobs_.push_back(std::move(job));
    }
    ready_.notify_one();
}

void ServiceWorker::loop() {
    while (true) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            ready_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
            if (stopping_) {
                return;
            }
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }
        Task done;
        try {
            done = job(cancel_);
        } catch (...) {
            done = nullptr; // Un trabajo que lanza no tumba el hilo.
        }
        post_([state = state_, done = std::move(done)] {
            if (!state->alive) {
                return;
            }
            --state->pending;
            if (done) {
                done();
            }
        });
    }
}

} // namespace chatbot::cli
