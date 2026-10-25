// Carpeta de datos del bot y ruta de la unidad de systemd (funciones puras).

#include "chatbot/addon_settings/paths.h"

#include "env_guard.hpp"

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>

namespace settings = chatbot::addon_settings;
using std::nullopt;

TEST_CASE("rutas: carpeta de datos del bot con y sin modo portable", "[addon_settings][rutas]") {
    const std::optional<std::string> home = "/home/ana";
    CHECK(settings::resolve_addon_data_dir("telegram", nullopt, nullopt, home) ==
          "/home/ana/.local/share/chatbot/telegram");
    CHECK(settings::resolve_addon_data_dir("telegram", nullopt, "/datos/xdg/", home) ==
          "/datos/xdg/chatbot/telegram");
    // XDG_DATA_HOME relativo se ignora (especificación XDG).
    CHECK(settings::resolve_addon_data_dir("telegram", nullopt, "relativo", home) ==
          "/home/ana/.local/share/chatbot/telegram");
    // El modo portable gana sobre XDG y HOME.
    CHECK(settings::resolve_addon_data_dir("telegram", "/usb/chatbot/datos", "/datos/xdg", home) ==
          "/usb/chatbot/datos/telegram");
    CHECK(settings::resolve_addon_data_dir("telegram", nullopt, nullopt, nullopt) == nullopt);
    CHECK(settings::invited_file_path("/x/telegram") == "/x/telegram/invitados.json");
}

TEST_CASE("rutas: la unidad de systemd de usuario", "[addon_settings][rutas]") {
    const std::string unit = "chatbot-telegram.service";
    CHECK(settings::resolve_unit_path(unit, "/home/ana") ==
          "/home/ana/.config/systemd/user/chatbot-telegram.service");
    CHECK(settings::resolve_unit_path(unit, nullopt) == nullopt);
    CHECK(settings::resolve_unit_path(unit, "") == nullopt);

    // XDG_CONFIG_HOME no cuenta: systemd --user no lo ve.
    const chatbot_test::EnvGuard home{"HOME", "/home/ana"};
    const chatbot_test::EnvGuard xdg{"XDG_CONFIG_HOME", "/cfg"};
    CHECK(settings::default_unit_path(unit) ==
          "/home/ana/.config/systemd/user/chatbot-telegram.service");
}
