// TelegramAddon sin FTXUI: lo guardado, guardar (secretos antes que la
// sección), los invitados y el servicio con procesos falsos y una carpeta
// temporal. Lo dibujado está en test_addons_panel.cpp.

#include "app_startup.h"
#include "telegram_addon.h"

#include "chatbot/addon_settings/invited.h"
#include "chatbot/addon_settings/paths.h"
#include "chatbot/addon_settings/systemd_unit.h"
#include "chatbot/credentials.h"

#include "blocking_transport.hpp"
#include "fake_system.hpp"
#include "temp_dir.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

namespace {

namespace settings = chatbot::addon_settings;
using chatbot::cli::AddonField;
using chatbot::cli::TelegramAddon;

constexpr const char* kToken = "123456789:AAFakeTokenForTestsOnly_abcdefghijk";
constexpr const char* kOtherToken = "987654321:AAOtherFakeTokenForTests_abcdefghij";
constexpr const char* kChatbot = "/home/ana/CB/build/chatbot";

std::string read_file(const std::filesystem::path& path) {
    std::ifstream file{path, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

struct Fixture {
    chatbot_test::ScopedTempDir dir;
    chatbot_test::TaskQueue queue;
    chatbot_test::FakeSystem system;
    bool installed = true;

    [[nodiscard]] std::filesystem::path config_path() const { return dir.path() / "config.json"; }
    [[nodiscard]] std::filesystem::path credentials_path() const {
        return dir.path() / "credentials.json";
    }
    [[nodiscard]] std::string data_dir() const { return (dir.path() / "telegram").string(); }
    [[nodiscard]] std::filesystem::path unit_path() const {
        return dir.path() / "systemd" / "chatbot-telegram.service";
    }

    void write_config(const nlohmann::json& telegram) const {
        const nlohmann::json app{{"model", "modelo-de-la-app"},
                                 {"addons", {{"telegram", telegram}}}};
        std::ofstream{config_path(), std::ios::binary} << app.dump(2);
    }
    void write_token() const {
        chatbot::Credentials credentials;
        credentials.keys["telegram"] = kToken;
        REQUIRE_FALSE(chatbot::save_credentials(credentials_path().string(), credentials));
    }

    chatbot::cli::AddonsEnvironment environment(chatbot::Os os = chatbot::Os::Posix) {
        chatbot::cli::AddonsEnvironment environment;
        environment.os = os;
        environment.post = [this](std::function<void()> task) { queue.post(std::move(task)); };
        environment.run = system.runner();
        environment.locate = [this](const settings::AddonInfo&) {
            return installed ? std::optional<std::string>{chatbot_test::kAddonExecutable}
                             : std::nullopt;
        };
        environment.config_path = config_path().string();
        environment.credentials_path = credentials_path().string();
        environment.data_dir = data_dir();
        environment.unit_path = unit_path().string();
        environment.uid = 1000;
        environment.chatbot_path = kChatbot;
        environment.home = "/home/ana";
        return environment;
    }

    /// Corre la tarea de un trabajo hasta que done sea true.
    void wait(const bool& done) { REQUIRE(queue.run_until([&] { return done; })); }

    void refresh(TelegramAddon& addon) {
        bool done = false;
        addon.refresh([&] { done = true; });
        wait(done);
    }
};

nlohmann::json enabled_section() {
    return {{"version", 1}, {"enabled", true}, {"allowed_users", {111111111}}};
}

} // namespace

TEST_CASE("TelegramAddon: lee la sección, los secretos enmascarables y los invitados",
          "[addons]") {
    Fixture fixture;
    fixture.write_config({{"version", 1}, {"enabled", true}, {"daily_message_limit", 20},
                          {"rara", 1}});
    fixture.write_token();
    REQUIRE(settings::add_invited(settings::invited_file_path(fixture.data_dir()), 42).is_ok());
    TelegramAddon addon{fixture.environment()};
    addon.load();
    CHECK(addon.load_error().empty());
    CHECK_FALSE(addon.read_only());
    CHECK(addon.saved_form().enabled);
    CHECK(addon.saved_form().at(AddonField::DailyLimit) == "20");
    CHECK(addon.saved_form().at(AddonField::Token).empty()); // Vacío: se conserva.
    CHECK(addon.secrets().token == kToken);
    CHECK(addon.discarded() == std::vector<std::string>{"rara"});
    CHECK(addon.discard_question() == "Se descartarán: rara. ¿Guardar? (s/n)");
    CHECK(addon.invited() == std::vector<std::int64_t>{42});
    CHECK(addon.invited_error().empty());
    CHECK_FALSE(addon.install().has_value());
    CHECK_FALSE(addon.service().has_value());
}

TEST_CASE("TelegramAddon: solo lectura con una sección más nueva o un addon incompatible",
          "[addons]") {
    Fixture fixture;
    fixture.write_token();
    SECTION("sección") {
        fixture.write_config({{"version", 2}, {"enabled", true}});
        TelegramAddon addon{fixture.environment()};
        addon.load();
        CHECK(addon.newer());
        CHECK(addon.read_only());
    }
    SECTION("programa") {
        fixture.write_config(enabled_section());
        fixture.system.answer(std::string{chatbot_test::kAddonExecutable} + " --config-version",
                              chatbot_test::exited(0, "2\n"));
        TelegramAddon addon{fixture.environment()};
        addon.load();
        CHECK_FALSE(addon.read_only());
        bool done = false;
        addon.check_install([&] { done = true; });
        fixture.wait(done);
        REQUIRE(addon.install().has_value());
        CHECK(addon.install()->config_version == 2);
        CHECK(addon.read_only());
    }
}

TEST_CASE("TelegramAddon: guardar escribe los secretos y después la sección", "[addons]") {
    Fixture fixture;
    fixture.write_config(enabled_section());
    fixture.write_token();
    TelegramAddon addon{fixture.environment()};
    addon.load();
    chatbot::cli::AddonForm form = addon.saved_form();
    form.at(AddonField::Token) = kOtherToken;
    form.at(AddonField::DailyLimit) = "7";
    REQUIRE_FALSE(chatbot::cli::addon_form_error(form, addon.secrets()).has_value());
    CHECK(addon.changed(form));

    SECTION("bien: lo escrito pasa a ser lo guardado") {
        REQUIRE_FALSE(addon.save(form).has_value());
        CHECK(addon.secrets().token == kOtherToken);
        CHECK(addon.saved_form().at(AddonField::Token).empty());
        CHECK(addon.saved_form().at(AddonField::DailyLimit) == "7");
        const chatbot::Result<chatbot::Credentials> saved =
            chatbot::load_credentials(fixture.credentials_path().string());
        REQUIRE(saved.is_ok());
        CHECK(saved.value().keys.at("telegram") == kOtherToken);
        CHECK(read_file(fixture.config_path()).find("\"daily_message_limit\": 7") !=
              std::string::npos);
        CHECK_FALSE(addon.changed(addon.saved_form()));
    }
    SECTION("si fallan los secretos, la sección no se escribe") {
        std::ofstream{fixture.credentials_path(), std::ios::binary | std::ios::trunc} << "{no";
        const std::string before = read_file(fixture.config_path());
        CHECK(addon.save(form).has_value());
        CHECK(read_file(fixture.config_path()) == before);
        CHECK(addon.secrets().token == kToken); // Lo guardado no cambió.
    }
}

TEST_CASE("TelegramAddon: reiniciar hace falta solo con el servicio activo", "[addons]") {
    Fixture fixture;
    fixture.write_config(enabled_section());
    fixture.write_token();
    TelegramAddon addon{fixture.environment()};
    addon.load();
    chatbot::cli::AddonForm form = addon.saved_form();
    form.at(AddonField::Token) = kOtherToken;
    CHECK_FALSE(addon.needs_restart(form)); // Aún no se sabe el estado.
    fixture.system.set_active(false);
    fixture.refresh(addon);
    CHECK_FALSE(addon.needs_restart(form));
    fixture.system.set_active(true);
    fixture.refresh(addon);
    CHECK(addon.needs_restart(form));
    form.enabled = false;
    CHECK_FALSE(addon.needs_restart(form));
}

TEST_CASE("TelegramAddon: quitar un invitado recarga solo si el servicio está activo",
          "[addons]") {
    Fixture fixture;
    fixture.write_config(enabled_section());
    fixture.write_token();
    const std::string path = settings::invited_file_path(fixture.data_dir());
    REQUIRE(settings::add_invited(path, 42).is_ok());
    REQUIRE(settings::add_invited(path, 43).is_ok());
    TelegramAddon addon{fixture.environment()};
    addon.load();
    const bool active = GENERATE(false, true);
    fixture.system.set_active(active);
    fixture.refresh(addon);
    fixture.system.clear();

    std::optional<TelegramAddon::Removal> removal;
    addon.remove_invited(42, [&](const TelegramAddon::Removal& done) { removal = done; });
    CHECK(addon.invited() == std::vector<std::int64_t>{43}); // Al momento.
    REQUIRE(fixture.queue.run_until([&] { return removal.has_value(); }));
    CHECK_FALSE(removal->error.has_value());
    if (active) {
        CHECK(removal->message == "Se quitó 42. Configuración aplicada al bot.");
        CHECK(chatbot_test::service_actions(fixture.system) ==
              std::vector<std::string>{"systemctl --user reload chatbot-telegram.service"});
    } else {
        CHECK(removal->message == "Se quitó 42.");
        CHECK(fixture.system.calls().empty());
    }
}

TEST_CASE("TelegramAddon: aplicar usa lo guardado y pregunta por una unidad ajena",
          "[addons]") {
    Fixture fixture;
    fixture.write_config(enabled_section());
    fixture.write_token();
    TelegramAddon addon{fixture.environment()};
    addon.load();

    SECTION("no instalado: no toca el servicio") {
        fixture.installed = false;
        std::optional<TelegramAddon::Applied> applied;
        addon.apply(false, false, [&](const TelegramAddon::Applied& done) { applied = done; });
        REQUIRE(fixture.queue.run_until([&] { return applied.has_value(); }));
        CHECK(applied->skipped.starts_with("No instalado: copia chatbot-telegram"));
        CHECK(chatbot_test::service_actions(fixture.system).empty());
        CHECK(addon.service().has_value());
    }
    SECTION("unidad ajena: Confirm y, con replace_foreign, la unidad de la app") {
        std::filesystem::create_directories(fixture.unit_path().parent_path());
        std::ofstream{fixture.unit_path(), std::ios::binary} << "[Service]\nExecStart=/bin/true\n";
        std::optional<TelegramAddon::Applied> applied;
        addon.apply(false, false, [&](const TelegramAddon::Applied& done) { applied = done; });
        REQUIRE(fixture.queue.run_until([&] { return applied.has_value(); }));
        CHECK(applied->action.kind == chatbot::cli::ServiceAction::Kind::Confirm);
        CHECK(applied->action.message == chatbot::cli::kReplaceForeignUnit);
        applied.reset();
        addon.apply(false, true, [&](const TelegramAddon::Applied& done) { applied = done; });
        REQUIRE(fixture.queue.run_until([&] { return applied.has_value(); }));
        CHECK(applied->action.kind == chatbot::cli::ServiceAction::Kind::Done);
        CHECK(read_file(fixture.unit_path()) == settings::unit_text(addon.unit_params()));
    }
}

TEST_CASE("TelegramAddon: en Windows no hay addon ni procesos", "[addons]") {
    Fixture fixture;
    fixture.write_config(enabled_section());
    TelegramAddon addon{fixture.environment(chatbot::Os::Windows)};
    addon.load();
    CHECK_FALSE(addon.available());
    CHECK_FALSE(addon.has_service());
    CHECK(addon.load_error().empty());
    CHECK_FALSE(addon.saved_form().enabled); // No se leyó nada.
    CHECK(fixture.system.calls().empty());
}

TEST_CASE("addons_environment: chatbot-rpc escribe la misma unidad que la TUI", "[addons]") {
    // El ExecStart es chatbot, no chatbot-rpc: si no, la TUI vería la unidad
    // de la GUI como vieja (stale) y la reescribiría cada vez. Solo Linux.
    if (chatbot::current_os() == chatbot::Os::Windows) {
        return;
    }
    const auto post = [](std::function<void()>) {};
    const std::string dir = "/opt/chatbot";
    CHECK(chatbot::cli::sibling_chatbot_path(dir + "/chatbot-rpc") == dir + "/chatbot");
    const TelegramAddon tui{chatbot::cli::addons_environment(post, dir + "/chatbot")};
    const TelegramAddon rpc{chatbot::cli::addons_environment(
        post, chatbot::cli::sibling_chatbot_path(dir + "/chatbot-rpc"))};
    CHECK(settings::unit_text(rpc.unit_params()) == settings::unit_text(tui.unit_params()));
    const std::string unit = settings::unit_text(rpc.unit_params());
    CHECK(unit.find("/opt/chatbot/chatbot") != std::string::npos);
    CHECK(unit.find("chatbot-rpc") == std::string::npos);
}
