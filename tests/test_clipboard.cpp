#include "clipboard.h"

#include <catch2/catch_test_macros.hpp>

#include <map>
#include <set>
#include <string>
#include <vector>

// Sin procesos reales: el entorno, el buscador de PATH, el lanzador y la
// terminal son falsos.

namespace {

using chatbot::Os;
using chatbot::cli::ClipboardMethod;
using chatbot::cli::CopyResult;

chatbot::cli::EnvLookup fake_env(std::map<std::string, std::string> values) {
    return [values = std::move(values)](std::string_view name) -> std::optional<std::string> {
        const auto found = values.find(std::string{name});
        return found != values.end() ? std::optional<std::string>{found->second} : std::nullopt;
    };
}

chatbot::cli::ProgramFinder fake_path(std::set<std::string> programs) {
    return [programs = std::move(programs)](std::string_view program) {
        return programs.count(std::string{program}) > 0;
    };
}

/// Nombres de los métodos, en orden ("OSC 52*" si es el último recurso).
std::vector<std::string> names(const std::vector<ClipboardMethod>& methods) {
    std::vector<std::string> out;
    for (const ClipboardMethod& method : methods) {
        out.push_back(method.name() + (method.fallback ? "*" : ""));
    }
    return out;
}

const std::set<std::string> kAll{"termux-clipboard-set", "wl-copy", "xclip", "xsel"};

} // namespace

TEST_CASE("base64 con los vectores del RFC 4648", "[clipboard]") {
    using chatbot::cli::base64_encode;
    CHECK(base64_encode("") == "");
    CHECK(base64_encode("f") == "Zg==");
    CHECK(base64_encode("fo") == "Zm8=");
    CHECK(base64_encode("foo") == "Zm9v");
    CHECK(base64_encode("foob") == "Zm9vYg==");
    CHECK(base64_encode("fooba") == "Zm9vYmE=");
    CHECK(base64_encode("foobar") == "Zm9vYmFy");
    // Bytes altos y UTF-8.
    CHECK(base64_encode(std::string{"\xff\xfe\x00", 3}) == "//4A");
    CHECK(base64_encode("ñ") == "w7E=");
}

TEST_CASE("secuencia OSC 52, sola y dentro de tmux", "[clipboard]") {
    CHECK(chatbot::cli::osc52_sequence("foobar", false) == "\x1b]52;c;Zm9vYmFy\x07");
    CHECK(chatbot::cli::osc52_sequence("foobar", true) ==
          "\x1bPtmux;\x1b\x1b]52;c;Zm9vYmFy\x07\x1b\\");
}

TEST_CASE("orden de los métodos según el entorno", "[clipboard]") {
    using chatbot::cli::clipboard_methods;

    SECTION("Sin nada: solo OSC 52 como último recurso") {
        CHECK(names(clipboard_methods(fake_env({}), fake_path(kAll), Os::Posix)) ==
              std::vector<std::string>{"OSC 52*"});
    }

    SECTION("Wayland") {
        const auto methods =
            clipboard_methods(fake_env({{"WAYLAND_DISPLAY", "wayland-1"}}), fake_path(kAll),
                              Os::Posix);
        CHECK(names(methods) == std::vector<std::string>{"wl-copy", "OSC 52*"});
        CHECK(methods.front().argv == std::vector<std::string>{"wl-copy"});
    }

    SECTION("X11: xclip y luego xsel, con sus argumentos fijos") {
        const auto methods =
            clipboard_methods(fake_env({{"DISPLAY", ":0"}}), fake_path(kAll), Os::Posix);
        REQUIRE(names(methods) == std::vector<std::string>{"xclip", "xsel", "OSC 52*"});
        CHECK(methods[0].argv == std::vector<std::string>{"xclip", "-selection", "clipboard"});
        CHECK(methods[1].argv == std::vector<std::string>{"xsel", "--clipboard", "--input"});
    }

    SECTION("Wayland con XWayland: primero wl-copy") {
        CHECK(names(clipboard_methods(fake_env({{"WAYLAND_DISPLAY", "w"}, {"DISPLAY", ":0"}}),
                                      fake_path(kAll), Os::Posix)) ==
              std::vector<std::string>{"wl-copy", "xclip", "xsel", "OSC 52*"});
    }

    SECTION("Termux") {
        CHECK(names(clipboard_methods(fake_env({{"TERMUX_VERSION", "0.118"}}), fake_path(kAll),
                                      Os::Posix)) ==
              std::vector<std::string>{"termux-clipboard-set", "OSC 52*"});
    }

    SECTION("Por SSH, OSC 52 va primero") {
        CHECK(names(clipboard_methods(fake_env({{"SSH_CONNECTION", "1.2.3.4 5 6.7.8.9 22"},
                                                {"DISPLAY", "localhost:10"}}),
                                      fake_path(kAll), Os::Posix)) ==
              std::vector<std::string>{"OSC 52", "xclip", "xsel", "OSC 52*"});
        CHECK(names(clipboard_methods(fake_env({{"SSH_TTY", "/dev/pts/1"}}), fake_path(kAll),
                                      Os::Posix)) ==
              std::vector<std::string>{"OSC 52", "OSC 52*"});
    }

    SECTION("Solo los programas que están en PATH") {
        CHECK(names(clipboard_methods(fake_env({{"WAYLAND_DISPLAY", "w"}, {"DISPLAY", ":0"}}),
                                      fake_path({"xsel"}), Os::Posix)) ==
              std::vector<std::string>{"xsel", "OSC 52*"});
    }

    SECTION("Una variable vacía no cuenta") {
        CHECK(names(clipboard_methods(fake_env({{"WAYLAND_DISPLAY", ""}, {"SSH_TTY", ""}}),
                                      fake_path(kAll), Os::Posix)) ==
              std::vector<std::string>{"OSC 52*"});
    }
}

TEST_CASE("copy_to_clipboard prueba los métodos en orden", "[clipboard]") {
    std::vector<std::vector<std::string>> launched;
    std::vector<std::string> inputs;
    std::set<std::string> working;
    const chatbot::cli::ProgramRunner run = [&](const std::vector<std::string>& argv,
                                                std::string_view input) {
        launched.push_back(argv);
        inputs.emplace_back(input);
        return working.count(argv.front()) > 0;
    };
    std::vector<std::string> written;
    bool terminal_ok = true;
    const chatbot::cli::TerminalWriter write = [&](std::string_view sequence) {
        written.emplace_back(sequence);
        return terminal_ok;
    };
    const auto methods = chatbot::cli::clipboard_methods(
        fake_env({{"WAYLAND_DISPLAY", "w"}, {"DISPLAY", ":0"}}), fake_path(kAll), Os::Posix);

    SECTION("El primero que funciona") {
        working = {"wl-copy"};
        const CopyResult result = copy_to_clipboard("int a;\n", methods, false, run, write);
        CHECK(result.copied);
        CHECK(result.method == "wl-copy");
        CHECK_FALSE(result.fallback);
        CHECK(launched.size() == 1);
        CHECK(inputs == std::vector<std::string>{"int a;\n"});
        CHECK(written.empty());
    }

    SECTION("Un programa que falla pasa al siguiente") {
        working = {"xsel"};
        const CopyResult result = copy_to_clipboard("x", methods, false, run, write);
        CHECK(result.copied);
        CHECK(result.method == "xsel");
        CHECK(launched.size() == 3);
        CHECK(written.empty());
    }

    SECTION("Si ninguno funciona, OSC 52 como último recurso") {
        const CopyResult result = copy_to_clipboard("foobar", methods, false, run, write);
        CHECK(result.copied);
        CHECK(result.method == "OSC 52");
        CHECK(result.fallback);
        CHECK(launched.size() == 3);
        CHECK(written == std::vector<std::string>{"\x1b]52;c;Zm9vYmFy\x07"});
    }

    SECTION("Dentro de tmux, OSC 52 va envuelto") {
        const CopyResult result = copy_to_clipboard("foobar", methods, true, run, write);
        CHECK(result.copied);
        CHECK(written == std::vector<std::string>{"\x1bPtmux;\x1b\x1b]52;c;Zm9vYmFy\x07\x1b\\"});
    }

    SECTION("Más de 100 000 bytes: sin OSC 52") {
        const std::string big(chatbot::cli::kOsc52MaxBytes + 1, 'a');
        const CopyResult result = copy_to_clipboard(big, methods, false, run, write);
        CHECK_FALSE(result.copied);
        CHECK(result.too_long);
        CHECK(written.empty());
        // Con 100 000 exactos sí se manda.
        const std::string limit(chatbot::cli::kOsc52MaxBytes, 'a');
        CHECK(copy_to_clipboard(limit, methods, false, run, write).copied);
        CHECK(written.size() == 1);
    }

    SECTION("Más de 100 000 bytes con un programa que funciona") {
        working = {"xclip"};
        const std::string big(chatbot::cli::kOsc52MaxBytes + 1, 'a');
        const CopyResult result = copy_to_clipboard(big, methods, false, run, write);
        CHECK(result.copied);
        CHECK(result.method == "xclip");
    }

    SECTION("La terminal no acepta la escritura") {
        terminal_ok = false;
        const CopyResult result = copy_to_clipboard("x", methods, false, run, write);
        CHECK_FALSE(result.copied);
        CHECK_FALSE(result.too_long);
    }

    SECTION("Por SSH, OSC 52 primero y no se lanza ningún programa") {
        const auto ssh = chatbot::cli::clipboard_methods(
            fake_env({{"SSH_TTY", "/dev/pts/0"}, {"DISPLAY", ":10"}}), fake_path(kAll), Os::Posix);
        working = {"xclip"};
        const CopyResult result = copy_to_clipboard("x", ssh, false, run, write);
        CHECK(result.copied);
        CHECK(result.method == "OSC 52");
        CHECK_FALSE(result.fallback);
        CHECK(launched.empty());
    }
}

TEST_CASE("orden de los métodos en Windows", "[clipboard][windows]") {
    using chatbot::cli::clipboard_methods;
    using Kind = ClipboardMethod::Kind;

    // Sin SSH: el portapapeles de Windows y OSC 52 como respaldo. Nunca
    // programas, aunque las variables de X11 o Wayland estén definidas.
    const auto local = clipboard_methods(
        fake_env({{"DISPLAY", ":0"}, {"WAYLAND_DISPLAY", "w"}, {"TERMUX_VERSION", "1"}}),
        fake_path(kAll), Os::Windows);
    REQUIRE(names(local) == std::vector<std::string>{"portapapeles de Windows", "OSC 52*"});
    CHECK(local[0].kind == Kind::Native);
    CHECK(local[1].kind == Kind::Osc52);

    // Por SSH, OSC 52 primero (igual que en POSIX).
    CHECK(names(clipboard_methods(fake_env({{"SSH_CONNECTION", "1.2.3.4 5 6.7.8.9 22"}}),
                                  fake_path(kAll), Os::Windows)) ==
          std::vector<std::string>{"OSC 52", "portapapeles de Windows", "OSC 52*"});
    CHECK(names(clipboard_methods(fake_env({{"SSH_TTY", ""}}), fake_path(kAll), Os::Windows)) ==
          std::vector<std::string>{"portapapeles de Windows", "OSC 52*"});
}

TEST_CASE("copy_to_clipboard con el portapapeles nativo", "[clipboard][windows]") {
    const auto methods = chatbot::cli::clipboard_methods(fake_env({}), fake_path({}), Os::Windows);
    std::vector<std::string> launched;
    const chatbot::cli::ProgramRunner run = [&](const std::vector<std::string>& argv,
                                                std::string_view) {
        launched.push_back(argv.front());
        return true;
    };
    std::vector<std::string> written;
    const chatbot::cli::TerminalWriter write = [&](std::string_view sequence) {
        written.emplace_back(sequence);
        return true;
    };
    std::vector<std::string> copied;
    bool native_ok = true;
    const chatbot::cli::NativeCopier native = [&](std::string_view text) {
        copied.emplace_back(text);
        return native_ok;
    };

    SECTION("Funciona: no se escribe OSC 52 ni se lanza nada") {
        const CopyResult result = copy_to_clipboard("año\n", methods, false, run, write, native);
        CHECK(result.copied);
        CHECK(result.method == "portapapeles de Windows");
        CHECK_FALSE(result.fallback);
        CHECK(copied == std::vector<std::string>{"año\n"});
        CHECK(written.empty());
        CHECK(launched.empty());
    }

    SECTION("Falla: OSC 52 como último recurso") {
        native_ok = false;
        const CopyResult result = copy_to_clipboard("foobar", methods, false, run, write, native);
        CHECK(result.copied);
        CHECK(result.method == "OSC 52");
        CHECK(result.fallback);
        CHECK(written == std::vector<std::string>{"\x1b]52;c;Zm9vYmFy\x07"});
    }

    SECTION("Sin copiador nativo, el método falla y sigue el siguiente") {
        const CopyResult result = copy_to_clipboard("foobar", methods, false, run, write);
        CHECK(result.copied);
        CHECK(result.method == "OSC 52");
        CHECK(copied.empty());
    }
}

TEST_CASE("to_crlf: saltos de línea de Windows", "[clipboard][windows]") {
    using chatbot::cli::to_crlf;
    CHECK(to_crlf("").empty());
    CHECK(to_crlf("sin saltos") == "sin saltos");
    CHECK(to_crlf("a\nb\n") == "a\r\nb\r\n");
    CHECK(to_crlf("\n\n") == "\r\n\r\n");
    // Los \r\n que ya estaban no se duplican; un \r suelto se queda igual.
    CHECK(to_crlf("a\r\nb\nc") == "a\r\nb\r\nc");
    CHECK(to_crlf("a\rb") == "a\rb");
    CHECK(to_crlf("año\nñandú\n") == "año\r\nñandú\r\n");
}
