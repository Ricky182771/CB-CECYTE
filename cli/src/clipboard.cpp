#include "clipboard.h"

#include <cstdlib>
#include <iostream>
#include <utility>

namespace chatbot::cli {

namespace {

bool defined(const EnvLookup& env, std::string_view name) {
    const std::optional<std::string> value = env(name);
    return value.has_value() && !value->empty();
}

ClipboardMethod program(std::vector<std::string> argv) {
    ClipboardMethod method;
    method.kind = ClipboardMethod::Kind::Program;
    method.argv = std::move(argv);
    return method;
}

ClipboardMethod native() {
    ClipboardMethod method;
    method.kind = ClipboardMethod::Kind::Native;
    return method;
}

ClipboardMethod osc52(bool fallback) {
    ClipboardMethod method;
    method.kind = ClipboardMethod::Kind::Osc52;
    method.fallback = fallback;
    return method;
}

} // namespace

std::string ClipboardMethod::name() const {
    if (kind == Kind::Osc52) {
        return "OSC 52";
    }
    if (kind == Kind::Native) {
        return "portapapeles de Windows";
    }
    return argv.empty() ? std::string{} : argv.front();
}

std::vector<ClipboardMethod> clipboard_methods(const EnvLookup& env, const ProgramFinder& find,
                                               Os os) {
    std::vector<ClipboardMethod> methods;
    if (defined(env, "SSH_CONNECTION") || defined(env, "SSH_TTY")) {
        methods.push_back(osc52(false));
    }
    if (os == Os::Windows) {
        // La API de Win32, sin lanzar programas (clip.exe revuelve el UTF-8
        // según la página de códigos).
        methods.push_back(native());
        methods.push_back(osc52(true));
        return methods;
    }
    const auto add = [&](std::vector<std::string> argv) {
        if (find(argv.front())) {
            methods.push_back(program(std::move(argv)));
        }
    };
    if (defined(env, "TERMUX_VERSION")) {
        add({"termux-clipboard-set"});
    }
    if (defined(env, "WAYLAND_DISPLAY")) {
        add({"wl-copy"});
    }
    if (defined(env, "DISPLAY")) {
        add({"xclip", "-selection", "clipboard"});
        add({"xsel", "--clipboard", "--input"});
    }
    methods.push_back(osc52(true));
    return methods;
}

std::string base64_encode(std::string_view data) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 3 <= data.size(); i += 3) {
        const unsigned value = static_cast<unsigned>(static_cast<unsigned char>(data[i])) << 16U |
                               static_cast<unsigned>(static_cast<unsigned char>(data[i + 1])) << 8U |
                               static_cast<unsigned>(static_cast<unsigned char>(data[i + 2]));
        out += kAlphabet[(value >> 18U) & 0x3FU];
        out += kAlphabet[(value >> 12U) & 0x3FU];
        out += kAlphabet[(value >> 6U) & 0x3FU];
        out += kAlphabet[value & 0x3FU];
    }
    const std::size_t rest = data.size() - i;
    if (rest > 0) {
        unsigned value = static_cast<unsigned>(static_cast<unsigned char>(data[i])) << 16U;
        if (rest == 2) {
            value |= static_cast<unsigned>(static_cast<unsigned char>(data[i + 1])) << 8U;
        }
        out += kAlphabet[(value >> 18U) & 0x3FU];
        out += kAlphabet[(value >> 12U) & 0x3FU];
        out += rest == 2 ? kAlphabet[(value >> 6U) & 0x3FU] : '=';
        out += '=';
    }
    return out;
}

std::string osc52_sequence(std::string_view text, bool tmux) {
    const std::string sequence = "\x1b]52;c;" + base64_encode(text) + "\x07";
    if (!tmux) {
        return sequence;
    }
    std::string wrapped = "\x1bPtmux;";
    for (const char c : sequence) {
        if (c == '\x1b') {
            wrapped += '\x1b'; // Dentro del passthrough de tmux, ESC va doble.
        }
        wrapped += c;
    }
    wrapped += "\x1b\\";
    return wrapped;
}

CopyResult copy_to_clipboard(std::string_view text, const std::vector<ClipboardMethod>& methods,
                             bool tmux, const ProgramRunner& run, const TerminalWriter& write,
                             const NativeCopier& native) {
    CopyResult result;
    for (const ClipboardMethod& method : methods) {
        if (method.kind == ClipboardMethod::Kind::Program) {
            if (!method.argv.empty() && run(method.argv, text)) {
                result.copied = true;
                result.method = method.name();
                return result;
            }
            continue;
        }
        if (method.kind == ClipboardMethod::Kind::Native) {
            if (native && native(text)) {
                result.copied = true;
                result.method = method.name();
                return result;
            }
            continue;
        }
        if (text.size() > kOsc52MaxBytes) {
            result.too_long = true;
            continue;
        }
        if (write(osc52_sequence(text, tmux))) {
            result.copied = true;
            result.method = method.name();
            result.fallback = method.fallback;
            return result;
        }
    }
    return result;
}

std::string to_crlf(std::string_view text) {
    std::string out;
    out.reserve(text.size() + text.size() / 16);
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\n' && (i == 0 || text[i - 1] != '\r')) {
            out += '\r';
        }
        out += text[i];
    }
    return out;
}

std::optional<std::string> environment_value(std::string_view name) {
    const char* value = std::getenv(std::string{name}.c_str());
    return value != nullptr ? std::optional<std::string>{value} : std::nullopt;
}

bool write_to_terminal(std::string_view sequence) {
    std::cout.write(sequence.data(), static_cast<std::streamsize>(sequence.size()));
    std::cout.flush();
    return !std::cout.fail();
}

} // namespace chatbot::cli
