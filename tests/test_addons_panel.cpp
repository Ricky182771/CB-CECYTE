// F2 → Addons dibujado y con eventos reales: el bloque de estado, los campos,
// guardar con el servicio, las preguntas en la línea de estado, los
// invitados y 80×24. Sin systemd ni procesos: un ProcessRunner falso, y los
// archivos en una carpeta temporal.

#include "settings_screen.h"

#include "chatbot/addon_settings/invited.h"
#include "chatbot/addon_settings/paths.h"
#include "chatbot/addon_settings/systemd_unit.h"
#include "chatbot/addon_settings/telegram_section.h"
#include "chatbot/addon_settings/token.h"
#include "chatbot/credentials.h"

#include "blocking_transport.hpp"
#include "fake_system.hpp"
#include "fake_transport.hpp"
#include "no_color_guard.hpp"
#include "temp_dir.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <ftxui/component/component_base.hpp>
#include <ftxui/screen/screen.hpp>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace {

namespace settings = chatbot::addon_settings;

constexpr const char* kToken = "123456789:AAFakeTokenForTestsOnly_abcdefghijk";
constexpr const char* kExecutable = chatbot_test::kAddonExecutable;

struct Position {
    int x = -1;
    int y = -1;
};

Position find(const ftxui::Screen& screen, const std::string& needle) {
    for (int y = 0; y < screen.dimy(); ++y) {
        for (int x = 0; x < screen.dimx(); ++x) {
            std::string text;
            for (int k = x; k < screen.dimx() && text.size() < needle.size(); ++k) {
                const auto& character = screen.CellAt(k, y).character;
                text += character.empty() ? " " : character;
            }
            if (text == needle) {
                return {x, y};
            }
        }
    }
    return {};
}

using chatbot_test::exited;
using chatbot_test::FakeSystem;

struct Harness {
    chatbot_test::ScopedTempDir dir;
    chatbot_test::TaskQueue queue;
    FakeSystem system;
    chatbot::cli::Palette palette{chatbot::cli::resolve_appearance("", "")};
    bool installed = true;
    /// open() escribe la unidad al día si no hay una.
    bool write_unit = true;
    chatbot::cli::ModelsLoader loader{[] {
                                          auto transport =
                                              std::make_unique<chatbot_test::FakeTransport>();
                                          chatbot::HttpResponse response;
                                          response.status = 200;
                                          response.body = R"({"data":[]})";
                                          transport->responses.push_back(std::move(response));
                                          return transport;
                                      },
                                      [this](auto task) { queue.post(std::move(task)); }};
    chatbot::cli::SettingsScreen settings{
        loader,
        palette,
        [](const chatbot::cli::ProviderSettings&) -> std::optional<std::string> {
            return std::nullopt;
        },
        [](const chatbot::cli::Appearance&) -> std::optional<std::string> { return std::nullopt; },
        [](const std::optional<std::string>&) -> std::optional<std::string> {
            return std::nullopt;
        },
        [](const std::string&) -> std::optional<std::string> { return std::nullopt; },
        [] {}};

    [[nodiscard]] std::string config_path() const { return (dir.path() / "config.json").string(); }
    [[nodiscard]] std::string credentials_path() const {
        return (dir.path() / "credentials.json").string();
    }
    [[nodiscard]] std::string data_dir() const { return (dir.path() / "telegram").string(); }
    [[nodiscard]] std::string unit_path() const {
        return (dir.path() / "systemd" / "chatbot-telegram.service").string();
    }
    [[nodiscard]] std::string invited_path() const {
        return settings::invited_file_path(data_dir());
    }

    void write_config(const nlohmann::json& telegram) const {
        const nlohmann::json app{{"provider", "custom"},
                                 {"base_url", "http://localhost:8080/v1"},
                                 {"model", "modelo-de-la-app"},
                                 {"addons", {{"telegram", telegram}}}};
        std::ofstream{config_path(), std::ios::binary} << app.dump(2);
    }

    [[nodiscard]] nlohmann::json section() const {
        std::ifstream file{config_path(), std::ios::binary};
        return nlohmann::json::parse(file).at("addons").at("telegram");
    }

    [[nodiscard]] chatbot::Credentials credentials() const {
        chatbot::Result<chatbot::Credentials> loaded = chatbot::load_credentials(credentials_path());
        REQUIRE(loaded.is_ok());
        return loaded.value();
    }

    /// La unidad que escribe la app con este entorno (chatbot_path: ExecStart).
    static std::string current_unit(std::string chatbot_path = "/home/ana/CB/build/chatbot") {
        const settings::AddonInfo& addon = *settings::find_addon("telegram");
        return settings::unit_text({std::move(chatbot_path), std::string{addon.name},
                                    std::string{addon.description}, std::nullopt, std::nullopt});
    }

    void put_unit(const std::string& text) const {
        std::filesystem::create_directories(std::filesystem::path{unit_path()}.parent_path());
        std::ofstream{unit_path(), std::ios::binary} << text;
    }

    [[nodiscard]] std::string unit_on_disk() const {
        std::ifstream unit{unit_path(), std::ios::binary};
        return std::string{std::istreambuf_iterator<char>{unit}, std::istreambuf_iterator<char>{}};
    }

    void open(chatbot::Os os = chatbot::Os::Posix,
              std::vector<std::string_view> env_defined = {}) {
        if (write_unit && !std::filesystem::exists(unit_path())) {
            put_unit(current_unit());
        }
        chatbot::cli::AddonsEnvironment environment;
        environment.os = os;
        // Como main.cpp: la tarea y después un Event::Custom para redibujar.
        environment.post = [this](std::function<void()> task) {
            queue.post([this, task = std::move(task)] {
                task();
                (void)settings.component()->OnEvent(ftxui::Event::Custom);
            });
        };
        environment.run = system.runner();
        environment.locate = [this](const settings::AddonInfo&) {
            return installed ? std::optional<std::string>{kExecutable} : std::nullopt;
        };
        environment.config_path = config_path();
        environment.credentials_path = credentials_path();
        environment.data_dir = data_dir();
        environment.unit_path = unit_path();
        environment.uid = 1000;
        environment.chatbot_path = "/home/ana/CB/build/chatbot";
        environment.env_defined = std::move(env_defined);
        environment.home = "/home/ana";
        settings.set_addons_environment(std::move(environment));
        if (!std::filesystem::exists(credentials_path())) {
            chatbot::Credentials credentials;
            credentials.keys["telegram"] = kToken;
            REQUIRE_FALSE(chatbot::save_credentials(credentials_path(), credentials).has_value());
        }

        chatbot::Config config;
        config.base_url = "http://localhost:8080/v1";
        config.model = "modelo-de-la-app";
        chatbot::cli::ProviderSettings provider({"custom", config.base_url, config.model}, {}, {});
        settings.open(std::move(provider), config, chatbot::cli::resolve_appearance("", ""),
                      "", chatbot::cli::SearchSettings{});
        REQUIRE(queue.run_until([this] { return !loader.busy(); }));
    }

    ftxui::Screen draw(int width = 80, int height = 24) {
        auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(width),
                                            ftxui::Dimension::Fixed(height));
        ftxui::Render(screen, settings.component()->Render());
        return screen;
    }

    bool click(const std::string& text, int width = 80, int height = 24) {
        const Position position = find(draw(width, height), text);
        if (position.x < 0) {
            return false;
        }
        ftxui::Mouse mouse;
        mouse.x = position.x;
        mouse.y = position.y;
        mouse.button = ftxui::Mouse::Left;
        mouse.motion = ftxui::Mouse::Pressed;
        return settings.component()->OnEvent(ftxui::Event::Mouse("", mouse));
    }

    bool key(const ftxui::Event& event) { return settings.component()->OnEvent(event); }

    /// Se ve text (en una pantalla grande: la línea de estado en una fila).
    bool shows(const std::string& text) { return find(draw(160, 60), text).x >= 0; }

    /// Corre lo que mande el hilo del servicio hasta que se vea text.
    bool wait_for_text(const std::string& text) {
        return queue.run_until([&] { return shows(text); });
    }

    /// Elige "Addons" y espera el estado.
    void go_to_addons() {
        REQUIRE(click("Addons"));
        REQUIRE(wait_for_text("Servicio: activo"));
    }

    /// Lleva el foco al campo cuya etiqueta es label, con Tab.
    void focus_field(const std::string& label) {
        for (int i = 0; i < 40; ++i) {
            REQUIRE(key(ftxui::Event::Tab));
            const ftxui::Screen screen = draw(120, 60);
            const Position at = find(screen, label);
            if (at.x >= 0 && screen.CellAt(at.x, at.y).background_color !=
                                 screen.CellAt(at.x - 1, at.y).background_color) {
                return;
            }
        }
        FAIL("no se llegó a " << label);
    }

    void type(const std::string& text) {
        for (const char c : text) {
            REQUIRE(key(ftxui::Event::Character(c)));
        }
    }

    void clear_field() {
        (void)key(ftxui::Event::End);
        for (int i = 0; i < 40; ++i) {
            (void)key(ftxui::Event::Backspace);
        }
    }

    /// Guarda con Enter sobre [ Guardar ] o con un clic (presionar y soltar).
    void save(bool by_click) {
        if (!by_click) {
            focus_field("[ Guardar ]");
            REQUIRE(key(ftxui::Event::Return));
            return;
        }
        const Position at = find(draw(160, 60), "[ Guardar ]");
        REQUIRE(at.x >= 0);
        ftxui::Mouse mouse;
        mouse.x = at.x;
        mouse.y = at.y;
        mouse.button = ftxui::Mouse::Left;
        mouse.motion = ftxui::Mouse::Pressed;
        REQUIRE(settings.component()->OnEvent(ftxui::Event::Mouse("", mouse)));
        mouse.motion = ftxui::Mouse::Released;
        (void)settings.component()->OnEvent(ftxui::Event::Mouse("", mouse));
    }

    /// Nada de esto contesta la pregunta abierta: el Event::Custom de cada
    /// post, Enter, Tab, Supr, otra letra y el ratón.
    void poke_question(const std::string& question) {
        for (const ftxui::Event& event :
             {ftxui::Event::Custom, ftxui::Event::Return, ftxui::Event::Tab,
              ftxui::Event::Delete, ftxui::Event::Character('x'), ftxui::Event::Character(' ')}) {
            CHECK(key(event));
            CHECK(shows(question));
        }
        (void)click("[ Guardar ]", 160, 60);
        (void)click(question.substr(0, 8), 160, 60);
        CHECK(shows(question));
    }

    /// Las acciones sobre el servicio que se corrieron (sin las consultas).
    [[nodiscard]] std::vector<std::string> service_actions() const {
        return chatbot_test::service_actions(system);
    }

};

nlohmann::json enabled_section() {
    return {{"version", 1},       {"enabled", true},
            {"allowed_users", {111111111}}, {"daily_message_limit", 50}};
}

} // namespace

TEST_CASE("addons: en Windows solo el aviso, sin procesos", "[ajustes][addons]") {
    const chatbot_test::NoColorGuard environment(nullptr);
    Harness h;
    h.write_config(enabled_section());
    h.open(chatbot::Os::Windows);
    REQUIRE(h.click("Addons"));
    const ftxui::Screen screen = h.draw();
    CHECK(find(screen, "Los addons todavía no están").x >= 0);
    CHECK(find(screen, "Token").x < 0);
    CHECK(h.system.calls().empty());
}

#ifndef _WIN32

TEST_CASE("addons: cabe en 80×24 sin encimarse", "[ajustes][addons]") {
    const chatbot_test::NoColorGuard environment(nullptr);
    Harness h;
    h.write_config(enabled_section());
    REQUIRE(settings::add_invited(h.invited_path(), 222222222).is_ok());
    h.open();
    h.go_to_addons();
    const ftxui::Screen screen = h.draw();
    const Position installed = find(screen, "Instalado: ~/CB/build/chatbot-telegram");
    const Position service = find(screen, "Servicio: activo · habilitado");
    const Position buttons = find(screen, "[ Reiniciar ] [ Actualizar estado ]");
    const Position linger = find(screen, "Sin linger: loginctl enable-linger \"$USER\"");
    const Position journal = find(screen, "Registro: journalctl --user -u chatbot-telegram");
    const Position activated = find(screen, "[x] Activado");
    const Position save = find(screen, "[ Guardar ]");
    REQUIRE(installed.x >= 0);
    REQUIRE(service.x >= 0);
    REQUIRE(buttons.x >= 0);
    REQUIRE(linger.x >= 0);
    REQUIRE(journal.x >= 0);
    REQUIRE(activated.x >= 0);
    REQUIRE(save.x >= 0);
    // Una fila cada uno, en orden, con los campos debajo del bloque fijo.
    CHECK(service.y == installed.y + 1);
    CHECK(buttons.y == service.y + 1);
    CHECK(linger.y == buttons.y + 1);
    CHECK(journal.y == linger.y + 1);
    CHECK(journal.y - installed.y + 1 <= 6);
    CHECK(activated.y > journal.y);
    CHECK(save.y > activated.y);
    CHECK(save.y == 21);
    // Se ve más de un campo a la vez.
    CHECK(find(screen, "Token").y > activated.y);
    CHECK(find(screen, "Modelo").y > activated.y);
}

TEST_CASE("addons: con la terminal baja, el foco siempre se ve", "[ajustes][addons]") {
    const chatbot_test::NoColorGuard environment(nullptr);
    Harness h;
    h.write_config(enabled_section());
    h.open();
    h.go_to_addons();
    for (const int height : {24, 16}) {
        INFO(height);
        h.focus_field("Zona horaria");
        CHECK(find(h.draw(80, height), "Zona horaria").x >= 0);
        CHECK(find(h.draw(80, height), "[ Guardar ]").x >= 0);
        h.focus_field("Token");
        CHECK(find(h.draw(80, height), "Token").x >= 0);
    }
}

TEST_CASE("addons: los botones responden al ratón", "[ajustes][addons][raton]") {
    const chatbot_test::NoColorGuard environment(nullptr);
    Harness h;
    h.write_config(enabled_section());
    h.open();
    h.go_to_addons();

    h.system.clear();
    REQUIRE(h.click("[ Actualizar estado ]"));
    REQUIRE(h.queue.run_until(
        [&] { return h.system.called("systemctl --user is-active chatbot-telegram.service"); }));

    REQUIRE(h.click("[ Reiniciar ]"));
    REQUIRE(h.wait_for_text("Bot reiniciado."));
    CHECK(h.system.called("systemctl --user restart chatbot-telegram.service"));

    // [Generar] llena el código (válido, sin error debajo).
    const ftxui::Screen tall = h.draw(120, 60);
    REQUIRE(find(tall, "[ Generar ]").x >= 0);
    REQUIRE(h.click("[ Generar ]", 120, 60));
    const ftxui::Screen after = h.draw(120, 60);
    CHECK(find(after, "8 a 64").x < 0);
    REQUIRE(h.click("[ Guardar ]", 120, 60));
    REQUIRE(h.wait_for_text("Guardado."));
    const std::string code = h.section().at("invite_code").get<std::string>();
    CHECK(code.size() == settings::kGeneratedInviteCodeLength);
}

TEST_CASE("addons: [Reiniciar] sin Activado guardado no hace nada", "[ajustes][addons]") {
    const chatbot_test::NoColorGuard environment(nullptr);
    Harness h;
    nlohmann::json section = enabled_section();
    section["enabled"] = false;
    h.write_config(section);
    h.open();
    h.go_to_addons();
    const auto dimmed = [&h] {
        const ftxui::Screen screen = h.draw(160, 60);
        const Position restart = find(screen, "[ Reiniciar ]");
        const Position refresh = find(screen, "[ Actualizar estado ]");
        REQUIRE(restart.x >= 0);
        REQUIRE(refresh.x >= 0);
        const auto& a = screen.CellAt(restart.x, restart.y);
        const auto& b = screen.CellAt(refresh.x, refresh.y);
        return a.foreground_color != b.foreground_color || a.dim != b.dim;
    };
    CHECK(dimmed());
    // Marcar Activado sin guardar no basta: cuenta lo guardado.
    h.focus_field("[ ] Activado");
    REQUIRE(h.key(ftxui::Event::Return));
    REQUIRE(h.shows("[x] Activado"));
    CHECK(dimmed());

    h.system.clear();
    REQUIRE(h.click("[ Reiniciar ]", 160, 60));
    CHECK(h.shows(std::string{chatbot::cli::kRestartNeedsEnabled}));
    CHECK_FALSE(h.system.called("systemctl --user restart chatbot-telegram.service"));

    h.save(false);
    REQUIRE(h.wait_for_text("Configuración aplicada al bot."));
    // Sin el puntero encima ni el foco, que lo pintan con la selección.
    ftxui::Mouse away;
    away.motion = ftxui::Mouse::Moved;
    (void)h.key(ftxui::Event::Mouse("", away));
    h.focus_field("Token");
    CHECK_FALSE(dimmed());
    h.system.clear();
    REQUIRE(h.click("[ Reiniciar ]", 160, 60));
    REQUIRE(h.wait_for_text("Bot reiniciado."));
    CHECK(h.system.called("systemctl --user restart chatbot-telegram.service"));
}

TEST_CASE("addons: guardar escribe la sección y recarga el bot", "[ajustes][addons]") {
    const chatbot_test::NoColorGuard environment(nullptr);
    Harness h;
    h.write_config(enabled_section());
    h.open();
    h.go_to_addons();
    h.focus_field("Mensajes por día");
    h.clear_field();
    h.type("0");
    REQUIRE(h.click("[ Guardar ]"));
    CHECK(h.shows("Mensajes por día:"));
    CHECK(h.section().at("daily_message_limit") == 50); // No se escribió nada.

    h.focus_field("Mensajes por día"); // El clic se llevó el foco a [Guardar].
    h.clear_field();
    h.type("30");
    h.system.clear();
    REQUIRE(h.click("[ Guardar ]"));
    REQUIRE(h.wait_for_text("Configuración aplicada al bot."));
    CHECK(h.section().at("daily_message_limit") == 30);
    CHECK(h.section().at("version") == 1);
    // Ya corría, habilitado y con la unidad al día: solo se recarga.
    CHECK(h.service_actions() ==
          std::vector<std::string>{"systemctl --user reload chatbot-telegram.service"});
    // Se quedó abierta y sin cambios: Esc cierra sin preguntar.
    CHECK(h.settings.is_open());
    REQUIRE(h.key(ftxui::Event::Escape));
    CHECK_FALSE(h.settings.is_open());
}

TEST_CASE("addons: desactivar detiene el servicio", "[ajustes][addons]") {
    const chatbot_test::NoColorGuard environment(nullptr);
    Harness h;
    h.write_config(enabled_section());
    h.open();
    h.go_to_addons();
    REQUIRE(h.click("[x] Activado"));
    REQUIRE(h.click("[ Guardar ]"));
    REQUIRE(h.wait_for_text("Servicio desactivado."));
    CHECK(h.section().at("enabled") == false);
    CHECK(h.system.called("systemctl --user disable --now chatbot-telegram.service"));
}

TEST_CASE("addons: con Activado, una unidad hecha a mano pregunta antes de reemplazarla",
          "[ajustes][addons]") {
    // El caso de la prueba a mano: Activado ya marcado, unidad sin la marca,
    // detenida y deshabilitada, y un campo cambiado.
    const chatbot_test::NoColorGuard environment(nullptr);
    const bool by_click = GENERATE(false, true);
    const char answer = GENERATE('s', 'n', 'e'); // 'e': Esc.
    INFO("clic=" << by_click << " respuesta=" << answer);
    Harness h;
    h.write_config(enabled_section());
    const std::string mine = "[Service]\nExecStart=/usr/local/bin/mi-bot\n";
    h.put_unit(mine);
    h.system.set_active(false);
    h.system.set_enabled(false);
    h.open();
    REQUIRE(h.click("Addons"));
    REQUIRE(h.wait_for_text("Servicio: detenido · deshabilitado · hecho a mano"));
    CHECK(h.shows("Activado, pero el servicio está hecho a mano: guarda para reemplazarlo."));
    h.focus_field("Mensajes por día");
    h.clear_field();
    h.type("20");
    h.system.clear();
    h.save(by_click);
    // La pregunta llega del hilo del servicio, con su Event::Custom detrás.
    REQUIRE(h.wait_for_text(std::string{chatbot::cli::kReplaceForeignUnit}));
    CHECK(h.section().at("daily_message_limit") == 20); // La sección sí se guardó.
    h.poke_question(std::string{chatbot::cli::kReplaceForeignUnit});
    CHECK(h.service_actions().empty());
    CHECK(h.unit_on_disk() == mine);

    REQUIRE(h.key(answer == 'e' ? ftxui::Event::Escape : ftxui::Event::Character(answer)));
    if (answer == 's') {
        REQUIRE(h.wait_for_text("Guardado. Servicio activado."));
        CHECK(h.unit_on_disk() == Harness::current_unit());
        CHECK(h.service_actions() ==
              std::vector<std::string>{"systemctl --user daemon-reload",
                                       "systemctl --user enable --now chatbot-telegram.service"});
    } else {
        REQUIRE(h.wait_for_text(answer == 'n'
                                    ? "Guardado. No se tocó el servicio existente."
                                    : "Cancelado: se guardó, pero no se tocó el servicio existente."));
        CHECK(h.unit_on_disk() == mine);
        CHECK(h.service_actions().empty());
    }
    CHECK_FALSE(h.shows(std::string{chatbot::cli::kReplaceForeignUnit}));
}

TEST_CASE("addons: con Activado, guardar sin cambios arranca el servicio detenido",
          "[ajustes][addons]") {
    const chatbot_test::NoColorGuard environment(nullptr);
    Harness h;
    h.write_config(enabled_section());
    h.system.set_active(false);
    h.system.set_enabled(false);
    h.open();
    REQUIRE(h.click("Addons"));
    REQUIRE(h.wait_for_text("Servicio: detenido · deshabilitado"));
    CHECK(h.shows("Activado, pero el servicio está detenido: guarda para arrancarlo."));
    h.system.clear();
    REQUIRE(h.click("[ Guardar ]"));
    REQUIRE(h.wait_for_text("Guardado. Servicio activado."));
    CHECK(h.service_actions() ==
          std::vector<std::string>{"systemctl --user enable --now chatbot-telegram.service"});
    CHECK(h.settings.is_open()); // Se queda abierta con el resultado.
}

TEST_CASE("addons: con Activado, una unidad con ExecStart viejo se reescribe",
          "[ajustes][addons]") {
    const chatbot_test::NoColorGuard environment(nullptr);
    Harness h;
    h.write_config(enabled_section());
    h.put_unit(Harness::current_unit("/carpeta/vieja/chatbot"));
    h.open();
    h.go_to_addons();
    CHECK(h.shows("Activado, pero el servicio está desactualizado: guarda para actualizarlo."));
    h.system.clear();
    REQUIRE(h.click("[ Guardar ]"));
    REQUIRE(h.wait_for_text("Configuración aplicada al bot."));
    CHECK(h.unit_on_disk() == Harness::current_unit());
    CHECK(h.service_actions() ==
          std::vector<std::string>{"systemctl --user daemon-reload",
                                   "systemctl --user reload chatbot-telegram.service"});
}

TEST_CASE("addons: sin Activado, el servicio que corre se detiene al guardar",
          "[ajustes][addons]") {
    const chatbot_test::NoColorGuard environment(nullptr);
    Harness h;
    nlohmann::json section = enabled_section();
    section["enabled"] = false;
    h.write_config(section);
    h.open();
    h.go_to_addons();
    CHECK(h.shows("Desactivado, pero el servicio sigue corriendo: guarda para detenerlo."));
    h.system.clear();
    REQUIRE(h.click("[ Guardar ]"));
    REQUIRE(h.wait_for_text("Guardado. Servicio desactivado."));
    CHECK(h.service_actions() ==
          std::vector<std::string>{"systemctl --user disable --now chatbot-telegram.service"});
}

TEST_CASE("addons: sin cambios y con el servicio como dice Activado, no se toca nada",
          "[ajustes][addons]") {
    const chatbot_test::NoColorGuard environment(nullptr);
    const bool enabled = GENERATE(true, false);
    Harness h;
    nlohmann::json section = enabled_section();
    section["enabled"] = enabled;
    h.write_config(section);
    h.system.set_active(enabled);
    h.system.set_enabled(enabled);
    h.open();
    REQUIRE(h.click("Addons"));
    REQUIRE(h.wait_for_text(enabled ? "Servicio: activo · habilitado"
                                    : "Servicio: detenido · deshabilitado"));
    CHECK_FALSE(h.shows("guarda para"));
    h.system.clear();
    REQUIRE(h.click("[ Guardar ]"));
    CHECK_FALSE(h.settings.is_open()); // Nada pendiente: guarda el proveedor y cierra.
    CHECK(h.service_actions().empty());
}

TEST_CASE("addons: cambiar el token pregunta si reiniciar", "[ajustes][addons]") {
    const chatbot_test::NoColorGuard environment(nullptr);
    const bool restart = GENERATE(true, false);
    Harness h;
    h.write_config(enabled_section());
    chatbot::Credentials credentials;
    credentials.keys["telegram"] = kToken;
    REQUIRE_FALSE(chatbot::save_credentials(h.credentials_path(), credentials).has_value());
    h.open();
    h.go_to_addons();
    h.focus_field("Token");
    h.type("987654321:AAOtroTokenFicticioDePrueba_xyz12345");
    h.save(GENERATE(false, true));
    const std::string question = "Este cambio necesita reiniciar el bot";
    CHECK(h.shows(question));
    CHECK(h.credentials().keys.at("telegram") == "987654321:AAOtroTokenFicticioDePrueba_xyz12345");
    h.system.clear();
    h.poke_question(question);
    CHECK(h.service_actions().empty());
    REQUIRE(h.key(ftxui::Event::Character(restart ? 'S' : 'N')));
    REQUIRE(h.wait_for_text(restart ? "Bot reiniciado." : "Configuración aplicada al bot."));
    CHECK(h.system.called("systemctl --user restart chatbot-telegram.service") == restart);
    // El token nunca se dibuja.
    const ftxui::Screen screen = h.draw(120, 60);
    CHECK(find(screen, "AAOtroToken").x < 0);
    CHECK(find(screen, "guardada: …2345").x >= 0);
}

TEST_CASE("addons: [Quitar] borra la key propia al guardar", "[ajustes][addons]") {
    const chatbot_test::NoColorGuard environment(nullptr);
    Harness h;
    h.write_config(enabled_section());
    chatbot::Credentials credentials;
    credentials.keys["telegram"] = kToken;
    credentials.keys["telegram:api_key"] = "nvapi-clave-propia-de-prueba-9876";
    REQUIRE_FALSE(chatbot::save_credentials(h.credentials_path(), credentials).has_value());
    h.open();
    h.go_to_addons();
    CHECK(find(h.draw(120, 60), "guardada: …9876").x >= 0);
    REQUIRE(h.click("[ Quitar ]", 120, 60));
    CHECK(find(h.draw(120, 60), "se quita: se usará la de la app").x >= 0);
    REQUIRE(h.click("[ Guardar ]", 120, 60));
    REQUIRE(h.wait_for_text("Guardado."));
    CHECK(h.credentials().keys.count("telegram:api_key") == 0);
    CHECK(h.credentials().keys.at("telegram") == kToken);
}

TEST_CASE("addons: Supr sobre un invitado lo quita al momento y recarga", "[ajustes][addons]") {
    const chatbot_test::NoColorGuard environment(nullptr);
    Harness h;
    h.write_config(enabled_section());
    REQUIRE(settings::add_invited(h.invited_path(), 222222222).is_ok());
    REQUIRE(settings::add_invited(h.invited_path(), 333333333).is_ok());
    h.open();
    h.go_to_addons();
    REQUIRE(h.click("222222222", 120, 60));
    REQUIRE(h.key(ftxui::Event::Delete));
    const std::string question = "¿Quitar 222222222 ahora? No espera a Guardar. (s/n)";
    CHECK(h.shows(question));
    h.poke_question(question);
    REQUIRE(h.key(ftxui::Event::Escape));
    CHECK(h.shows("Cancelado: no se quitó a nadie."));
    CHECK(settings::load_invited(h.invited_path()).value().size() == 2);
    REQUIRE(h.key(ftxui::Event::Delete));
    REQUIRE(h.shows(question));
    h.system.clear();
    REQUIRE(h.key(ftxui::Event::Character('s')));
    REQUIRE(h.wait_for_text("Se quitó 222222222."));
    CHECK(settings::load_invited(h.invited_path()).value() == std::vector<std::int64_t>{333333333});
    REQUIRE(h.queue.run_until(
        [&] { return h.system.called("systemctl --user reload chatbot-telegram.service"); }));
    CHECK(find(h.draw(120, 60), "Invitados (1)").x >= 0);
    // Cancelar no lo deshace.
    REQUIRE(h.key(ftxui::Event::Escape));
    CHECK(settings::load_invited(h.invited_path()).value() == std::vector<std::int64_t>{333333333});
}

TEST_CASE("addons: invitados.json dañado se dice en lugar de la lista", "[ajustes][addons]") {
    const chatbot_test::NoColorGuard environment(nullptr);
    Harness h;
    h.write_config(enabled_section());
    std::filesystem::create_directories(h.data_dir());
    std::ofstream{h.invited_path()} << "{ roto";
    h.system.set_active(false);
    h.system.answer("systemctl --user show -p ExecMainStatus,Result chatbot-telegram.service",
                    exited(0, "ExecMainStatus=65\nResult=exit-code\n"));
    h.open();
    REQUIRE(h.click("Addons"));
    REQUIRE(h.wait_for_text("Servicio: detenido"));
    const ftxui::Screen screen = h.draw(160, 60);
    CHECK(find(screen, "invitados.json está dañado: " + h.invited_path()).x >= 0);
    CHECK(find(screen, "Invitados (").x < 0);
}

TEST_CASE("addons: incompatible o de una versión nueva, solo lectura", "[ajustes][addons]") {
    const chatbot_test::NoColorGuard environment(nullptr);
    Harness h;
    SECTION("versión del programa") {
        h.write_config(enabled_section());
        h.system.answer(std::string{kExecutable} + " --config-version", exited(0, "2\n"));
        h.open();
        h.go_to_addons();
        CHECK(find(h.draw(160, 60), "Incompatible: esta app usa la versión 1").x >= 0);
    }
    SECTION("versión de la sección") {
        nlohmann::json newer = enabled_section();
        newer["version"] = 2;
        h.write_config(newer);
        h.open();
        h.go_to_addons();
        CHECK(find(h.draw(160, 60), "versión más nueva (2)").x >= 0);
    }
    // Sin campos que editar ni nada que guardar.
    h.focus_field("[ Actualizar estado ]");
    REQUIRE(h.key(ftxui::Event::Tab));
    (void)h.key(ftxui::Event::Character('9'));
    REQUIRE(h.key(ftxui::Event::Escape));
    CHECK_FALSE(h.settings.is_open());
}

TEST_CASE("addons: llaves desconocidas piden confirmar", "[ajustes][addons]") {
    const chatbot_test::NoColorGuard environment(nullptr);
    Harness h;
    nlohmann::json section = enabled_section();
    section["extra"] = 1;
    h.write_config(section);
    h.open();
    h.go_to_addons();
    h.focus_field("Mensajes por día");
    h.clear_field();
    h.type("20");
    h.save(GENERATE(false, true));
    const std::string question = "Se descartarán: extra. ¿Guardar? (s/n)";
    CHECK(h.shows(question));
    h.poke_question(question);
    CHECK(h.section().contains("extra"));
    const char answer = GENERATE('s', 'n', 'e'); // 'e': Esc.
    REQUIRE(h.key(answer == 'e' ? ftxui::Event::Escape : ftxui::Event::Character(answer)));
    if (answer == 's') {
        REQUIRE(h.wait_for_text("Guardado."));
        CHECK_FALSE(h.section().contains("extra"));
        CHECK(h.section().at("daily_message_limit") == 20);
    } else {
        CHECK_FALSE(h.shows(question));
        CHECK(h.shows("Cancelado: no se guardó nada.") == (answer == 'e'));
        CHECK(h.section().contains("extra"));
        CHECK(h.section().at("daily_message_limit") == 50);
    }
}

TEST_CASE("addons: no instalado y variables de entorno", "[ajustes][addons]") {
    const chatbot_test::NoColorGuard environment(nullptr);
    Harness h;
    h.installed = false;
    h.write_config(enabled_section());
    h.open(chatbot::Os::Posix, {"CHAT_API_KEY"});
    REQUIRE(h.click("Addons"));
    REQUIRE(h.wait_for_text("Servicio: activo"));
    const ftxui::Screen screen = h.draw(160, 60);
    CHECK(find(screen, "No instalado: copia chatbot-telegram junto a chatbot o en el PATH.").x >=
          0);
    CHECK(find(screen, "CHAT_API_KEY está definida en esta terminal").x >= 0);
    // Se puede configurar antes de instalarlo; el servicio no se toca.
    h.focus_field("Mensajes por día");
    h.clear_field();
    h.type("20");
    REQUIRE(h.click("[ Guardar ]", 160, 60));
    REQUIRE(h.wait_for_text("Guardado. No instalado"));
    CHECK_FALSE(h.system.called("systemctl --user enable --now chatbot-telegram.service"));
}

#endif
