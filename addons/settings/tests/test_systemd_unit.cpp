// Texto de la unidad de systemd, escape de ExecStart= y Environment= y
// estado de una unidad que ya existe.

#include "chatbot/addon_settings/systemd_unit.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

namespace settings = chatbot::addon_settings;

namespace {

settings::UnitParams params() {
    settings::UnitParams p;
    p.chatbot_path = "/home/ana/CB-CECYTE/build/release/cli/chatbot";
    p.addon = "telegram";
    p.description = "Bot de Telegram del chatbot de CECyTE";
    return p;
}

} // namespace

TEST_CASE("unidad: el texto completo, sin Environment=", "[addon_settings][systemd]") {
    CHECK(settings::unit_text(params()) ==
          "# Generado por chatbot (Configuración → Addons). No lo edites: se reescribe desde "
          "ahí.\n"
          "[Unit]\n"
          "Description=Bot de Telegram del chatbot de CECyTE\n"
          "\n"
          "[Service]\n"
          "Type=exec\n"
          "ExecStart=\"/home/ana/CB-CECYTE/build/release/cli/chatbot\" \"telegram\"\n"
          "ExecReload=kill -HUP $MAINPID\n"
          "Restart=on-failure\n"
          "RestartSec=10\n"
          "RestartPreventExitStatus=65 69 73 78\n"
          "\n"
          "[Install]\n"
          "WantedBy=default.target\n");
}

TEST_CASE("unidad: Environment= solo con XDG_* absolutos", "[addon_settings][systemd]") {
    settings::UnitParams p = params();
    p.xdg_config_home = "/home/ana/mi config";
    p.xdg_data_home = "relativo/datos"; // Se ignora, como en la app.
    const std::string text = settings::unit_text(p);
    CHECK(text.find("Environment=\"XDG_CONFIG_HOME=/home/ana/mi config\"\n") != std::string::npos);
    CHECK(text.find("XDG_DATA_HOME") == std::string::npos);
    p.xdg_data_home = "/datos";
    CHECK(settings::unit_text(p).find("Environment=\"XDG_DATA_HOME=/datos\"\n\n[Install]") !=
          std::string::npos);
}

TEST_CASE("unidad: escape de rutas con espacios, %, $, comillas y controles",
          "[addon_settings][systemd]") {
    CHECK(settings::quote_exec_argument("/home/ana/Mis cosas/chatbot") ==
          "\"/home/ana/Mis cosas/chatbot\"");
    CHECK(settings::quote_exec_argument("/a/100%/b") == "\"/a/100%%/b\"");
    CHECK(settings::quote_exec_argument("/a/$HOME/b") == "\"/a/$$HOME/b\"");
    CHECK(settings::quote_exec_argument("/a/\"x\"/b") == "\"/a/\\\"x\\\"/b\"");
    CHECK(settings::quote_exec_argument("/a\\b") == "\"/a\\\\b\"");
    CHECK(settings::quote_exec_argument("/a\nb\tc") == "\"/a\\x0ab\\x09c\"");
    CHECK(settings::quote_exec_argument("/ñandú/chatbot") == "\"/ñandú/chatbot\"");
    // En Environment= no hay variables: $ queda igual; % sí es especificador.
    CHECK(settings::quote_environment("XDG_CONFIG_HOME", "/a b/$x/50%/\"q\"") ==
          "\"XDG_CONFIG_HOME=/a b/$x/50%%/\\\"q\\\"\"");

    settings::UnitParams p = params();
    p.chatbot_path = "/home/ana/100% $real/\"chat\"bot";
    CHECK(settings::unit_text(p).find(
              "ExecStart=\"/home/ana/100%% $$real/\\\"chat\\\"bot\" \"telegram\"\n") !=
          std::string::npos);
}

TEST_CASE("unidad: ajena, al día o vieja", "[addon_settings][systemd]") {
    const settings::UnitParams p = params();
    CHECK(settings::classify_unit(std::nullopt, p) == settings::UnitState::Missing);
    CHECK(settings::classify_unit(settings::unit_text(p), p) == settings::UnitState::Current);

    // Una hecha a mano con el README: sin la marca.
    const std::string hand_made = "[Unit]\nDescription=Bot\n\n[Service]\nExecStart=/usr/bin/x\n";
    CHECK(settings::classify_unit(hand_made, p) == settings::UnitState::Foreign);
    CHECK(settings::classify_unit("", p) == settings::UnitState::Foreign);
    // La marca tiene que ser la primera línea completa.
    CHECK(settings::classify_unit(std::string{settings::kUnitMarker} + " y algo\n", p) ==
          settings::UnitState::Foreign);

    // Se movió la carpeta de chatbot: ExecStart viejo.
    settings::UnitParams moved = p;
    moved.chatbot_path = "/opt/chatbot/chatbot";
    CHECK(settings::classify_unit(settings::unit_text(p), moved) == settings::UnitState::Stale);
    // Cambió XDG_CONFIG_HOME.
    settings::UnitParams xdg = p;
    xdg.xdg_config_home = "/otra";
    CHECK(settings::classify_unit(settings::unit_text(p), xdg) == settings::UnitState::Stale);
}
