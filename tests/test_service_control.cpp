// El servicio de systemd de usuario de un addon: la unidad en disco,
// systemctl, loginctl y journalctl con un run_process falso (nunca el
// systemd real), y el hilo de ServiceWorker.

#include "service_control.h"

#include "chatbot/addon_settings/registry.h"
#include "chatbot/addon_settings/systemd_unit.h"

#include "temp_dir.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace {

namespace settings = chatbot::addon_settings;
using chatbot::cli::ProcessOptions;
using chatbot::cli::ProcessResult;
using chatbot::cli::ServiceAction;
using chatbot::cli::ServiceControl;

ProcessResult exited(int code, std::string out = {}, std::string err = {}) {
    ProcessResult result;
    result.status = ProcessResult::Status::Exited;
    result.exit_code = code;
    result.out = std::move(out);
    result.err = std::move(err);
    return result;
}

/// Contesta según los argumentos (unidos con espacios) y anota cada llamada.
struct FakeRunner {
    std::map<std::string, ProcessResult> answers;
    ProcessResult otherwise = exited(0);
    std::vector<std::string> calls;
    std::vector<std::chrono::milliseconds> timeouts;

    chatbot::cli::ProcessRunner runner() {
        return [this](const std::vector<std::string>& argv, const ProcessOptions& options) {
            std::string joined;
            for (const std::string& part : argv) {
                joined += (joined.empty() ? "" : " ") + part;
            }
            calls.push_back(joined);
            timeouts.push_back(options.timeout);
            const auto found = answers.find(joined);
            return found != answers.end() ? found->second : otherwise;
        };
    }
};

const settings::AddonInfo& telegram() {
    const settings::AddonInfo* addon = settings::find_addon("telegram");
    REQUIRE(addon != nullptr);
    return *addon;
}

settings::UnitParams params(std::string chatbot = "/opt/chatbot/chatbot") {
    return settings::UnitParams{std::move(chatbot), "telegram",
                                "Bot de Telegram del chatbot de CECyTE", std::nullopt,
                                std::nullopt};
}

std::string read(const std::filesystem::path& path) {
    std::ifstream file{path, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

struct ServiceHarness {
    chatbot_test::ScopedTempDir dir;
    std::filesystem::path unit = dir.path() / "systemd" / "user" / "chatbot-telegram.service";
    FakeRunner fake;
    ServiceControl control{telegram(), unit.string(), 1000UL, fake.runner()};
};

} // namespace

TEST_CASE("servicio: mensajes de los códigos de salida", "[servicio]") {
    using chatbot::cli::exit_status_message;
    CHECK(exit_status_message(65, "chatbot-telegram", "/d/telegram") ==
          "invitados.json está dañado: /d/telegram/invitados.json. Arréglalo o quítalo de ahí.");
    CHECK(exit_status_message(69, "chatbot-telegram", "/d") == "No se encontró chatbot-telegram");
    CHECK(exit_status_message(73, "chatbot-telegram", "/d/telegram") ==
          "No se puede escribir en la carpeta de datos del bot: /d/telegram.");
    CHECK(exit_status_message(78, "chatbot-telegram", "/d") ==
          "Error de configuración: revisa los campos de arriba");
    CHECK(exit_status_message(1, "chatbot-telegram", "/d").empty());
}

TEST_CASE("servicio: show, la última línea de error y el detalle quitado", "[servicio]") {
    CHECK(chatbot::cli::show_property("ExecMainStatus=78\nResult=exit-code\n", "Result") ==
          "exit-code");
    CHECK(chatbot::cli::show_property("ExecMainStatus=78\n", "ExecMain") == std::nullopt);

    const std::string journal =
        "2026-10-24 10:00:00 info inicio respuestas_simultaneas=4\n"
        "2026-10-24 10:01:00 error error chat=5 usuario=5 origen=modelo tipo=\"servidor\" "
        "http=500 ms=20 detalle=\"el alumno escribió: mi contraseña es 1234\"\n"
        "2026-10-24 10:02:00 aviso enviado_como_texto chat=5\n"
        "2026-10-24 10:03:00 error telegram_error metodo=sendMessage chat=5 http=400 "
        "tipo=\"petición inválida\" detalle=\"Bad Request: texto del alumno\"\n"
        "2026-10-24 10:04:00 info fin\n";
    const std::string last = chatbot::cli::last_error_line(journal, "chatbot-telegram");
    CHECK(last ==
          "telegram_error metodo=sendMessage chat=5 http=400 tipo=\"petición inválida\"");
    CHECK(last.find("alumno") == std::string::npos);
    CHECK(chatbot::cli::last_error_line(journal + "chatbot-telegram: Falta el token de "
                                                  "Telegram.\n",
                                        "chatbot-telegram") == "Falta el token de Telegram.");
    CHECK(chatbot::cli::last_error_line("2026-10-24 10:00:00 info fin\n", "chatbot-telegram")
              .empty());
    // Ninguna línea de error deja ver su detalle, esté donde esté.
    CHECK(chatbot::cli::last_error_line("2026-10-24 10:00:00 error x detalle=\"secreto\"",
                                        "chatbot-telegram") == "x");
    CHECK(chatbot::cli::last_error_line("2026-10-24 10:00:00 error detalle=\"secreto\"",
                                        "chatbot-telegram")
              .empty());
}

TEST_CASE("servicio: el estado con systemctl, loginctl y journalctl", "[servicio]") {
    ServiceHarness h;
    h.fake.answers["systemctl --user is-active chatbot-telegram.service"] = exited(3, "failed\n");
    h.fake.answers["systemctl --user is-enabled chatbot-telegram.service"] =
        exited(0, "enabled\n");
    h.fake.answers["systemctl --user show -p ExecMainStatus,Result chatbot-telegram.service"] =
        exited(0, "ExecMainStatus=78\nResult=exit-code\n");
    h.fake.answers["loginctl show-user 1000 -p Linger --value"] = exited(0, "no\n");
    h.fake.answers["journalctl --user -u chatbot-telegram.service -o cat -n 200 --no-pager"] =
        exited(0, "2026-10-24 10:00:00 error telegram_token_invalido http=401\n");
    const chatbot::cli::ServiceStatus status = h.control.status(params());
    CHECK(status.systemd);
    CHECK(status.active == "failed");
    CHECK(status.enabled == "enabled");
    CHECK(status.exit_status == 78);
    CHECK(status.unit == settings::UnitState::Missing);
    CHECK(status.linger == false);
    CHECK(status.last_error == "telegram_token_invalido http=401");
    for (const auto timeout : h.fake.timeouts) {
        CHECK(timeout == chatbot::cli::kServiceCommandTimeout);
    }

    // Corriendo: ExecMainStatus=0 no es un error; linger encendido.
    h.fake.answers["systemctl --user show -p ExecMainStatus,Result chatbot-telegram.service"] =
        exited(0, "ExecMainStatus=0\nResult=success\n");
    h.fake.answers["loginctl show-user 1000 -p Linger --value"] = exited(0, "yes\n");
    const chatbot::cli::ServiceStatus running = h.control.status(params());
    CHECK_FALSE(running.exit_status.has_value());
    CHECK(running.linger == true);
}

TEST_CASE("servicio: sin systemctl o sin bus de usuario", "[servicio]") {
    ServiceHarness h;
    ProcessResult missing;
    missing.status = ProcessResult::Status::NotFound;
    h.fake.otherwise = missing;
    CHECK_FALSE(h.control.status(params()).systemd);
    CHECK(h.control.apply(params(), true, false, false).kind == ServiceAction::Kind::NoSystemd);
    CHECK(h.control.apply(params(), false, false, false).kind == ServiceAction::Kind::NoSystemd);
    CHECK(h.control.disable().message == chatbot::cli::kNoUserSystemd);

    h.fake.otherwise =
        exited(1, "", "Failed to connect to user scope bus via local transport: No such file\n");
    CHECK_FALSE(h.control.status(params()).systemd);
    CHECK(h.control.reload_if_active().kind == ServiceAction::Kind::NoSystemd);
}

namespace {

constexpr const char* kIsActive = "systemctl --user is-active chatbot-telegram.service";
constexpr const char* kIsEnabled = "systemctl --user is-enabled chatbot-telegram.service";
constexpr const char* kEnableNow = "systemctl --user enable --now chatbot-telegram.service";

/// is-active e is-enabled del servicio falso.
void set_service(ServiceHarness& h, bool running, bool enabled) {
    h.fake.answers[kIsActive] = running ? exited(0, "active\n") : exited(3, "inactive\n");
    h.fake.answers[kIsEnabled] = enabled ? exited(0, "enabled\n") : exited(1, "disabled\n");
}

void write_unit(ServiceHarness& h, const std::string& text) {
    std::filesystem::create_directories(h.unit.parent_path());
    std::ofstream{h.unit, std::ios::binary} << text;
}

} // namespace

TEST_CASE("servicio: con Activado, sin unidad la escribe, recarga systemd y la habilita",
          "[servicio]") {
    ServiceHarness h;
    set_service(h, false, false);
    const ServiceAction done = h.control.apply(params(), true, false, false);
    CHECK(done.kind == ServiceAction::Kind::Done);
    CHECK(done.message == "Servicio activado.");
    CHECK(read(h.unit) == settings::unit_text(params()));
    CHECK(h.fake.calls == std::vector<std::string>{kIsActive, kIsEnabled,
                                                   "systemctl --user daemon-reload", kEnableNow});
}

TEST_CASE("servicio: con Activado, nuestra, detenida y deshabilitada: enable --now",
          "[servicio]") {
    ServiceHarness h;
    write_unit(h, settings::unit_text(params()));
    const bool running = GENERATE(false, true);
    const bool enabled = GENERATE(false, true);
    if (running && enabled) {
        return; // Ese caso es el de recargar.
    }
    INFO("running=" << running << " enabled=" << enabled);
    set_service(h, running, enabled);
    CHECK(h.control.apply(params(), true, false, false).kind == ServiceAction::Kind::Done);
    // Ya está al día: no se reescribe ni se recarga systemd.
    std::vector<std::string> expected{kIsActive, kIsEnabled, kEnableNow};
    if (running) {
        expected.emplace_back("systemctl --user reload chatbot-telegram.service");
    }
    CHECK(h.fake.calls == expected);
}

TEST_CASE("servicio: con Activado y ya corriendo: reload o restart", "[servicio]") {
    ServiceHarness h;
    write_unit(h, settings::unit_text(params()));
    set_service(h, true, true);
    CHECK(h.control.apply(params(), true, false, false).message ==
          "Configuración aplicada al bot.");
    CHECK(h.fake.calls == std::vector<std::string>{
                              kIsActive, kIsEnabled,
                              "systemctl --user reload chatbot-telegram.service"});
    h.fake.calls.clear();
    CHECK(h.control.apply(params(), true, false, true).message == "Bot reiniciado.");
    CHECK(h.fake.calls.back() == "systemctl --user restart chatbot-telegram.service");
}

TEST_CASE("servicio: con Activado, una unidad con ExecStart viejo se reescribe", "[servicio]") {
    ServiceHarness h;
    write_unit(h, settings::unit_text(params()));
    set_service(h, false, false);
    CHECK(h.control.status(params("/nueva/carpeta/chatbot")).unit == settings::UnitState::Stale);
    h.fake.calls.clear();
    CHECK(h.control.apply(params("/nueva/carpeta/chatbot"), true, false, false).kind ==
          ServiceAction::Kind::Done);
    CHECK(read(h.unit) == settings::unit_text(params("/nueva/carpeta/chatbot")));
    CHECK(h.fake.calls == std::vector<std::string>{kIsActive, kIsEnabled,
                                                   "systemctl --user daemon-reload", kEnableNow});
}

TEST_CASE("servicio: una unidad ajena no se reemplaza sin confirmar", "[servicio]") {
    ServiceHarness h;
    const std::string mine = "[Service]\nExecStart=/usr/local/bin/chatbot-telegram\n";
    write_unit(h, mine);
    set_service(h, false, false);
    CHECK(h.control.status(params()).unit == settings::UnitState::Foreign);
    h.fake.calls.clear();
    const ServiceAction ask = h.control.apply(params(), true, false, false);
    CHECK(ask.kind == ServiceAction::Kind::Confirm);
    CHECK(ask.message == chatbot::cli::kReplaceForeignUnit);
    CHECK(read(h.unit) == mine);
    CHECK(h.fake.calls == std::vector<std::string>{kIsActive, kIsEnabled}); // Solo preguntó.

    h.fake.calls.clear();
    CHECK(h.control.apply(params(), true, true, false).kind == ServiceAction::Kind::Done);
    CHECK(read(h.unit) == settings::unit_text(params()));
    CHECK(h.fake.calls == std::vector<std::string>{kIsActive, kIsEnabled,
                                                   "systemctl --user daemon-reload", kEnableNow});
}

TEST_CASE("servicio: sin Activado, disable --now solo si corre o está habilitado",
          "[servicio]") {
    ServiceHarness h;
    const bool running = GENERATE(false, true);
    const bool enabled = GENERATE(false, true);
    INFO("running=" << running << " enabled=" << enabled);
    set_service(h, running, enabled);
    const ServiceAction action = h.control.apply(params(), false, false, false);
    CHECK(action.kind == ServiceAction::Kind::Done);
    std::vector<std::string> expected{kIsActive, kIsEnabled};
    if (running || enabled) {
        expected.emplace_back("systemctl --user disable --now chatbot-telegram.service");
        CHECK(action.message == "Servicio desactivado.");
    } else {
        CHECK(action.message.empty());
    }
    CHECK(h.fake.calls == expected);
    CHECK_FALSE(std::filesystem::exists(h.unit)); // Nunca escribe la unidad.
}

TEST_CASE("servicio: qué cuenta como corriendo", "[servicio]") {
    CHECK(chatbot::cli::service_running("active"));
    CHECK(chatbot::cli::service_running("activating"));
    CHECK(chatbot::cli::service_running("reloading"));
    CHECK_FALSE(chatbot::cli::service_running("inactive"));
    CHECK_FALSE(chatbot::cli::service_running("failed"));
    CHECK_FALSE(chatbot::cli::service_running(""));
}

TEST_CASE("servicio: desactivar, reiniciar, recargar y sus errores", "[servicio]") {
    ServiceHarness h;
    CHECK(h.control.disable().message == "Servicio desactivado.");
    CHECK(h.control.restart().message == "Bot reiniciado.");
    CHECK(h.fake.calls == std::vector<std::string>{
                              "systemctl --user disable --now chatbot-telegram.service",
                              "systemctl --user restart chatbot-telegram.service"});

    // Recargar solo si corre.
    h.fake.calls.clear();
    h.fake.answers["systemctl --user is-active chatbot-telegram.service"] =
        exited(3, "inactive\n");
    CHECK(h.control.reload_if_active().kind == ServiceAction::Kind::Done);
    CHECK(h.fake.calls.size() == 1);
    h.fake.answers["systemctl --user is-active chatbot-telegram.service"] = exited(0, "active\n");
    CHECK(h.control.reload_if_active().message == "Configuración aplicada al bot.");
    CHECK(h.fake.calls.back() == "systemctl --user reload chatbot-telegram.service");

    h.fake.answers["systemctl --user restart chatbot-telegram.service"] =
        exited(1, "", "Job for chatbot-telegram.service failed.\nSee details\n");
    const ServiceAction failed = h.control.restart();
    CHECK(failed.kind == ServiceAction::Kind::Failed);
    CHECK(failed.message == "No se pudo reiniciar el bot: Job for chatbot-telegram.service failed.");

    ProcessResult hung;
    hung.status = ProcessResult::Status::TimedOut;
    h.fake.answers["systemctl --user disable --now chatbot-telegram.service"] = hung;
    CHECK(h.control.disable().message == "No se pudo desactivar el servicio: no respondió en 5 s");
}

TEST_CASE("servicio: ServiceWorker corre en orden y entrega por post", "[servicio][hilos]") {
    std::mutex mutex;
    std::vector<std::function<void()>> posted;
    std::condition_variable changed;
    std::vector<int> order;
    {
        chatbot::cli::ServiceWorker worker{[&](std::function<void()> task) {
            const std::lock_guard<std::mutex> lock(mutex);
            posted.push_back(std::move(task));
            changed.notify_all();
        }};
        for (int i = 0; i < 3; ++i) {
            worker.push([&order, i](const chatbot::CancelToken&) { return std::function<void()>{[&order, i] { order.push_back(i); }}; });
        }
        CHECK(worker.busy());
        {
            std::unique_lock<std::mutex> lock(mutex);
            REQUIRE(changed.wait_for(lock, std::chrono::seconds{5},
                                     [&] { return posted.size() == 3; }));
        }
        for (const auto& task : posted) {
            task(); // En el "hilo de la interfaz".
        }
        CHECK(order == std::vector<int>{0, 1, 2});
        CHECK_FALSE(worker.busy());
        posted.clear();
        worker.push([&order](const chatbot::CancelToken&) { return std::function<void()>{[&order] { order.push_back(9); }}; });
        std::unique_lock<std::mutex> lock(mutex);
        REQUIRE(changed.wait_for(lock, std::chrono::seconds{5}, [&] { return !posted.empty(); }));
    }
    // Destruido: lo que quedaba en la cola de post ya no corre.
    posted.front()();
    CHECK(order == std::vector<int>{0, 1, 2});
}

#ifndef _WIN32
TEST_CASE("servicio: cerrar con una consulta colgada no espera el tope", "[servicio][hilos]") {
    const chatbot_test::ScopedTempDir dir;
    std::atomic<int> runs{0};
    chatbot::CancelToken started;
    // Cada programa de la consulta es un sleep 60 de verdad.
    const ServiceControl control{
        telegram(), (dir.path() / "chatbot-telegram.service").string(), 1000UL,
        [&](const std::vector<std::string>&, const ProcessOptions& options) {
            ++runs;
            started.cancel();
            return chatbot::cli::run_process({"sleep", "60"}, options);
        }};
    std::optional<chatbot::cli::ServiceWorker> worker;
    worker.emplace([](std::function<void()>) {});
    worker->push([&](const chatbot::CancelToken& cancel) {
        (void)control.status(params(), &cancel);
        return std::function<void()>{};
    });
    REQUIRE(started.wait_for(std::chrono::seconds{5}));
    const auto begin = std::chrono::steady_clock::now();
    worker.reset();
    const auto elapsed = std::chrono::steady_clock::now() - begin;
    const double ms = std::chrono::duration<double, std::milli>(elapsed).count();
    WARN("destruir ServiceWorker: " << ms << " ms");
    CHECK(elapsed < std::chrono::seconds{1});
    CHECK(runs == 1); // No siguió con is-enabled, show, loginctl ni journalctl.
}
#endif
