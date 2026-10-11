#include <catch2/catch_test_macros.hpp>

#include "key_log.h"
#include "temp_dir.hpp"

#include <fstream>
#include <iterator>
#include <string>

using chatbot::cli::format_key_event;
using chatbot::cli::format_paste_shortcut;
using chatbot::cli::hex_bytes;
using chatbot::cli::KeyLog;
using chatbot::cli::KeyLogEvent;
using chatbot::cli::KeyLogState;
using chatbot::cli::PasteTarget;

namespace {

std::string read_all(const std::filesystem::path& path) {
    std::ifstream file{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

} // namespace

TEST_CASE("hex_bytes escribe cada byte en hex", "[key_log]") {
    CHECK(hex_bytes("\x16") == "16");
    CHECK(hex_bytes("\x1B[2;2~") == "1b 5b 32 3b 32 7e");
    CHECK(hex_bytes("") == "");
    CHECK(hex_bytes("\xFF\x80") == "ff 80");
}

TEST_CASE("Una línea de carácter nunca lleva el texto", "[key_log]") {
    const std::string secret = "nvapi-SECRETO-1234";
    KeyLogEvent event;
    event.kind = KeyLogEvent::Kind::Character;
    event.input = secret;
    KeyLogState state;
    state.settings_open = true;
    state.paste_target = PasteTarget::SingleLine;
    state.focused_fields = {{"key", true}};
    const std::string line = format_key_event("12:00:00.000", event, state);
    CHECK(line.find("len=18") != std::string::npos);
    CHECK(line.find("SECRETO") == std::string::npos);
    CHECK(line.find("nvapi") == std::string::npos);
    CHECK(line.find("1234") == std::string::npos);
    // Tampoco en hex.
    CHECK(line.find(hex_bytes("SEC")) == std::string::npos);
    CHECK(line.find(hex_bytes("nvapi")) == std::string::npos);

    SECTION("ni con un solo carácter") {
        event.input = "Z";
        const std::string one = format_key_event("12:00:00.000", event, state);
        CHECK(one.find('Z') == std::string::npos);
        CHECK(one.find("5a") == std::string::npos);
        CHECK(one.find("len=1") != std::string::npos);
    }
}

TEST_CASE("format_key_event describe el evento y el estado", "[key_log]") {
    KeyLogState state;
    state.settings_open = true;
    state.sidebar_focused = false;
    state.paste_target = PasteTarget::None;
    state.bracketed_paste_open = true;
    state.focused_fields = {{"url", false}, {"key", true}};

    KeyLogEvent special;
    special.kind = KeyLogEvent::Kind::Special;
    special.input = "\x16";
    CHECK(format_key_event("10:11:12.345", special, state) ==
          "10:11:12.345 special [16] | settings=open sidebar_focus=no target=none "
          "bracketed=open focused: url=0 key=1");

    KeyLogEvent mouse;
    mouse.kind = KeyLogEvent::Kind::Mouse;
    mouse.button = "right";
    mouse.motion = "pressed";
    mouse.x = 3;
    mouse.y = 7;
    KeyLogState chat;
    chat.paste_target = PasteTarget::MultiLine;
    CHECK(format_key_event("t", mouse, chat) ==
          "t mouse right pressed x=3 y=7 | settings=closed sidebar_focus=no "
          "target=multi-line bracketed=closed");

    KeyLogEvent custom;
    custom.kind = KeyLogEvent::Kind::Custom;
    CHECK(format_key_event("t", custom, chat).rfind("t custom |", 0) == 0);
}

TEST_CASE("format_paste_shortcut da la longitud del portapapeles, no el texto", "[key_log]") {
    CHECK(format_paste_shortcut("t", std::nullopt, false) ==
          "t paste-shortcut clipboard=nullopt delivered=no");
    CHECK(format_paste_shortcut("t", 42, true) == "t paste-shortcut clipboard=len=42 delivered=yes");
}

TEST_CASE("key_log_time tiene hora con milisegundos", "[key_log]") {
    const std::string time = chatbot::cli::key_log_time(std::chrono::system_clock::now());
    REQUIRE(time.size() == 12);
    CHECK(time[2] == ':');
    CHECK(time[5] == ':');
    CHECK(time[8] == '.');
}

TEST_CASE("KeyLog agrega líneas y sin ruta no hace nada", "[key_log]") {
    const chatbot_test::ScopedTempDir dir;
    const std::filesystem::path path = dir.path() / "keys.txt";
    {
        KeyLog log{path.string()};
        REQUIRE(log.enabled());
        log.write("uno");
        log.write("dos");
    }
    {
        KeyLog log{path.string()};
        log.write("tres");
    }
    CHECK(read_all(path) == "uno\ndos\ntres\n");

    KeyLog none{std::nullopt};
    CHECK_FALSE(none.enabled());
    none.write("nada"); // No truena.
    KeyLog empty{std::string{}};
    CHECK_FALSE(empty.enabled());
}

TEST_CASE("KeyLog con una ruta que no se puede abrir sigue sin avisar", "[key_log]") {
    const chatbot_test::ScopedTempDir dir;
    // Una carpeta que no existe: no se puede crear el archivo.
    KeyLog log{(dir.path() / "no-existe" / "keys.txt").string()};
    CHECK_FALSE(log.enabled());
    log.write("x");
    CHECK_FALSE(std::filesystem::exists(dir.path() / "no-existe"));
}
