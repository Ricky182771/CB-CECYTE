// La línea de comandos de chatbot: argumentos y búsqueda del addon (las
// funciones puras; el exec real lo prueba addon_launcher.cmake).

#include "addon_launcher.h"

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <set>
#include <string>
#include <vector>

namespace {

using chatbot::cli::CommandLine;
using chatbot::cli::find_addon_executable;
using chatbot::cli::parse_command_line;

const chatbot::addon_settings::AddonInfo& telegram() {
    return *chatbot::addon_settings::find_addon("telegram");
}

} // namespace

TEST_CASE("línea de comandos: interfaz, ayuda, addon y error de uso", "[addons][lanzador]") {
    CHECK(parse_command_line({}).kind == CommandLine::Kind::Interface);
    CHECK(parse_command_line({"--help"}).kind == CommandLine::Kind::Help);
    CHECK(parse_command_line({"-h"}).kind == CommandLine::Kind::Help);
    CHECK(parse_command_line({"--help", "telegram"}).kind == CommandLine::Kind::Usage);

    const CommandLine addon = parse_command_line({"telegram", "--help", "a b", ""});
    REQUIRE(addon.kind == CommandLine::Kind::Addon);
    CHECK(addon.addon == &telegram());
    CHECK(addon.arguments == std::vector<std::string>{"--help", "a b", ""});

    const CommandLine unknown = parse_command_line({"Telegram"});
    CHECK(unknown.kind == CommandLine::Kind::Usage);
    CHECK(unknown.problem == "argumento desconocido: Telegram");
    CHECK(parse_command_line({"--version"}).kind == CommandLine::Kind::Usage);
}

TEST_CASE("línea de comandos: el addon junto a chatbot, luego PATH; portable sin PATH",
          "[addons][lanzador]") {
    std::set<std::string> executables;
    std::vector<std::string> searched;
    const auto is_executable = [&](const std::string& path) { return executables.count(path) > 0; };
    const auto search = [&](std::string_view program) -> std::optional<std::string> {
        searched.emplace_back(program);
        return "/usr/bin/" + std::string{program};
    };

    executables.insert("/opt/app/chatbot-telegram");
    CHECK(find_addon_executable(telegram(), "/opt/app", false, is_executable, search) ==
          "/opt/app/chatbot-telegram");
    CHECK(searched.empty());

    executables.clear();
    CHECK(find_addon_executable(telegram(), "/opt/app", false, is_executable, search) ==
          "/usr/bin/chatbot-telegram");
    CHECK(searched == std::vector<std::string>{"chatbot-telegram"});

    searched.clear();
    CHECK(find_addon_executable(telegram(), "/opt/app", true, is_executable, search) ==
          std::nullopt);
    CHECK(searched.empty());
}

TEST_CASE("línea de comandos: mensajes y ayuda", "[addons][lanzador]") {
    CHECK(chatbot::cli::addon_not_found_message(telegram(), false) ==
          "No encontré chatbot-telegram junto a chatbot ni en el PATH.");
    CHECK(chatbot::cli::addon_not_found_message(telegram(), true) ==
          "No encontré chatbot-telegram junto a chatbot.");

    const auto found = [](const chatbot::addon_settings::AddonInfo&) {
        return std::optional<std::string>{"/opt/app/chatbot-telegram"};
    };
    const auto missing = [](const chatbot::addon_settings::AddonInfo&) {
        return std::optional<std::string>{};
    };
    const std::string installed = chatbot::cli::usage_text(chatbot::Os::Posix, false, found);
    CHECK(installed.find("telegram  Telegram: instalado en /opt/app/chatbot-telegram\n") !=
          std::string::npos);
    CHECK(chatbot::cli::usage_text(chatbot::Os::Posix, false, missing)
              .find("no instalado (copia chatbot-telegram junto a chatbot o en el PATH)") !=
          std::string::npos);
    CHECK(chatbot::cli::usage_text(chatbot::Os::Posix, true, missing)
              .find("no instalado (copia chatbot-telegram junto a chatbot)") != std::string::npos);

    bool searched = false;
    const std::string windows = chatbot::cli::usage_text(
        chatbot::Os::Windows, false, [&](const chatbot::addon_settings::AddonInfo&) {
            searched = true;
            return std::optional<std::string>{};
        });
    CHECK_FALSE(searched);
    CHECK(windows.find("Los addons todavía no están disponibles en Windows.") !=
          std::string::npos);
}
