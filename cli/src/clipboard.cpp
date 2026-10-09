#include "clipboard.h"

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <thread>
#include <utility>

#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ; // NOLINT: lo exige posix_spawnp.

namespace chatbot::cli {

namespace {

using Clock = std::chrono::steady_clock;

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

ClipboardMethod osc52(bool fallback) {
    ClipboardMethod method;
    method.kind = ClipboardMethod::Kind::Osc52;
    method.fallback = fallback;
    return method;
}

/// Descriptor que se cierra solo.
class Fd {
public:
    explicit Fd(int fd) : fd_(fd) {}
    ~Fd() { reset(); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    [[nodiscard]] int get() const { return fd_; }
    void reset() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

private:
    int fd_;
};

/// Milisegundos que faltan para deadline (0 si ya pasó), como int para poll.
int remaining_ms(Clock::time_point deadline) {
    const auto left =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
    if (left <= 0) {
        return 0;
    }
    return left > 60000 ? 60000 : static_cast<int>(left);
}

/// Escribe todo input en fd (no bloqueante) antes de deadline. SIGPIPE se
/// bloquea en este hilo mientras tanto: si el hijo sale sin leer, write da
/// EPIPE en vez de matar al chatbot; la señal pendiente se descarta.
bool write_all(int fd, std::string_view input, Clock::time_point deadline) {
    sigset_t pipe_set;
    sigemptyset(&pipe_set);
    sigaddset(&pipe_set, SIGPIPE);
    sigset_t old_set;
    if (pthread_sigmask(SIG_BLOCK, &pipe_set, &old_set) != 0) {
        return false;
    }
    const int flags = ::fcntl(fd, F_GETFL);
    bool ok = flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
    while (ok && !input.empty()) {
        const ssize_t written = ::write(fd, input.data(), input.size());
        if (written > 0) {
            input.remove_prefix(static_cast<std::size_t>(written));
            continue;
        }
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            pollfd waiting{fd, POLLOUT, 0};
            const int ready = ::poll(&waiting, 1, remaining_ms(deadline));
            ok = ready > 0 || (ready < 0 && errno == EINTR);
            continue;
        }
        ok = false; // EPIPE u otro error.
    }
    sigset_t pending;
    if (sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE) == 1 &&
        sigismember(&old_set, SIGPIPE) == 0) {
        const timespec zero{};
        (void)sigtimedwait(&pipe_set, nullptr, &zero);
    }
    (void)pthread_sigmask(SIG_SETMASK, &old_set, nullptr);
    return ok;
}

/// Espera al hijo hasta deadline; si se pasa, lo mata. true si salió con 0.
bool wait_child(pid_t pid, Clock::time_point deadline) {
    int status = 0;
    while (true) {
        const pid_t done = ::waitpid(pid, &status, WNOHANG);
        if (done == pid) {
            return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        }
        if (done < 0 && errno != EINTR) {
            return false;
        }
        if (Clock::now() >= deadline) {
            ::kill(pid, SIGKILL);
            while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
            }
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

} // namespace

std::string ClipboardMethod::name() const {
    if (kind == Kind::Osc52) {
        return "OSC 52";
    }
    return argv.empty() ? std::string{} : argv.front();
}

std::vector<ClipboardMethod> clipboard_methods(const EnvLookup& env, const ProgramFinder& find) {
    std::vector<ClipboardMethod> methods;
    if (defined(env, "SSH_CONNECTION") || defined(env, "SSH_TTY")) {
        methods.push_back(osc52(false));
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
                             bool tmux, const ProgramRunner& run, const TerminalWriter& write) {
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

std::optional<std::string> environment_value(std::string_view name) {
    const char* value = std::getenv(std::string{name}.c_str());
    return value != nullptr ? std::optional<std::string>{value} : std::nullopt;
}

bool find_in_path(std::string_view program) {
    const std::optional<std::string> path = environment_value("PATH");
    if (!path.has_value() || program.empty() || program.find('/') != std::string_view::npos) {
        return false;
    }
    std::string_view rest = *path;
    while (true) {
        const std::size_t end = rest.find(':');
        const std::string_view dir = rest.substr(0, end);
        // Una carpeta vacía sería la actual: no se usa.
        if (!dir.empty()) {
            const std::string candidate = std::string{dir} + "/" + std::string{program};
            struct stat info{};
            if (::stat(candidate.c_str(), &info) == 0 && S_ISREG(info.st_mode) &&
                ::access(candidate.c_str(), X_OK) == 0) {
                return true;
            }
        }
        if (end == std::string_view::npos) {
            return false;
        }
        rest.remove_prefix(end + 1);
    }
}

bool run_with_input(const std::vector<std::string>& argv, std::string_view input,
                    std::chrono::milliseconds timeout) {
    if (argv.empty()) {
        return false;
    }
    const Clock::time_point deadline = Clock::now() + timeout;
    int fds[2] = {-1, -1};
    if (::pipe2(fds, O_CLOEXEC) != 0) {
        return false;
    }
    Fd read_end(fds[0]);
    Fd write_end(fds[1]);

    // stdin del hijo = el pipe; stdout y stderr a /dev/null, para que no
    // ensucien la pantalla de FTXUI. Los demás descriptores del pipe tienen
    // O_CLOEXEC.
    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions) != 0) {
        return false;
    }
    bool ready = posix_spawn_file_actions_adddup2(&actions, read_end.get(), STDIN_FILENO) == 0 &&
                 posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY,
                                                  0) == 0 &&
                 posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY,
                                                  0) == 0;
    // El hijo empieza sin señales bloqueadas y con SIGPIPE en su acción por
    // defecto, sin importar lo que tenga este hilo.
    posix_spawnattr_t attributes;
    const bool have_attributes = posix_spawnattr_init(&attributes) == 0;
    ready = ready && have_attributes;
    if (ready) {
        sigset_t none;
        sigemptyset(&none);
        sigset_t defaults;
        sigemptyset(&defaults);
        sigaddset(&defaults, SIGPIPE);
        ready = posix_spawnattr_setsigmask(&attributes, &none) == 0 &&
                posix_spawnattr_setsigdefault(&attributes, &defaults) == 0 &&
                posix_spawnattr_setflags(&attributes, static_cast<short>(POSIX_SPAWN_SETSIGMASK |
                                                                         POSIX_SPAWN_SETSIGDEF)) ==
                    0;
    }
    pid_t pid = -1;
    if (ready) {
        std::vector<std::string> args = argv;
        std::vector<char*> c_args;
        c_args.reserve(args.size() + 1);
        for (std::string& arg : args) {
            c_args.push_back(arg.data());
        }
        c_args.push_back(nullptr);
        ready = posix_spawnp(&pid, c_args.front(), &actions, &attributes, c_args.data(),
                             environ) == 0;
    }
    posix_spawn_file_actions_destroy(&actions);
    if (have_attributes) {
        posix_spawnattr_destroy(&attributes);
    }
    read_end.reset();
    if (!ready) {
        return false;
    }
    const bool written = write_all(write_end.get(), input, deadline);
    write_end.reset(); // EOF para el hijo.
    const bool exited_ok = wait_child(pid, deadline);
    return written && exited_ok;
}

bool write_to_terminal(std::string_view sequence) {
    std::cout.write(sequence.data(), static_cast<std::streamsize>(sequence.size()));
    std::cout.flush();
    return !std::cout.fail();
}

} // namespace chatbot::cli
