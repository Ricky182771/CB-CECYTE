// Programas externos en POSIX: posix_spawnp (sin shell) con pipes para
// stdin, stdout y stderr, y la búsqueda en PATH. CMake lo compila en lugar
// de child_process_windows.cpp fuera de Windows.

#include "child_process.h"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <ctime>

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

/// Descriptor que se cierra solo.
class Fd {
public:
    explicit Fd(int fd = -1) : fd_(fd) {}
    ~Fd() { reset(); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    [[nodiscard]] int get() const { return fd_; }
    [[nodiscard]] bool open() const { return fd_ >= 0; }
    void reset(int fd = -1) {
        if (fd_ >= 0) {
            ::close(fd_);
        }
        fd_ = fd;
    }

private:
    int fd_;
};

/// Un pipe con O_CLOEXEC en las dos puntas: el hijo solo hereda la que se
/// le pasa con dup2.
bool make_pipe(Fd& read_end, Fd& write_end) {
    int fds[2] = {-1, -1};
    if (::pipe2(fds, O_CLOEXEC) != 0) {
        return false;
    }
    read_end.reset(fds[0]);
    write_end.reset(fds[1]);
    return true;
}

bool set_nonblocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL);
    return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

/// Milisegundos que faltan para deadline (0 si ya pasó), como int para poll.
int remaining_ms(Clock::time_point deadline) {
    const auto left =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
    if (left <= 0) {
        return 0;
    }
    return left > 60000 ? 60000 : static_cast<int>(left);
}

/// Lee lo que haya en fd sin bloquear; con EOF o error, lo cierra.
void read_available(Fd& fd, std::string& out) {
    char buffer[4096];
    while (fd.open()) {
        const ssize_t got = ::read(fd.get(), buffer, sizeof buffer);
        if (got > 0) {
            const std::size_t room = kMaxProcessOutput - std::min(out.size(), kMaxProcessOutput);
            out.append(buffer, std::min(room, static_cast<std::size_t>(got)));
            continue;
        }
        if (got < 0 && errno == EINTR) {
            continue;
        }
        if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return;
        }
        fd.reset(); // EOF o error.
    }
}

/// SIGPIPE bloqueada en este hilo mientras vive: si el hijo sale sin leer
/// stdin, write da EPIPE en vez de matar a la app. Al salir descarta la
/// señal pendiente y restaura la máscara.
class BlockSigpipe {
public:
    BlockSigpipe() {
        sigemptyset(&pipe_set_);
        sigaddset(&pipe_set_, SIGPIPE);
        active_ = pthread_sigmask(SIG_BLOCK, &pipe_set_, &old_set_) == 0;
    }
    ~BlockSigpipe() {
        if (!active_) {
            return;
        }
        sigset_t pending;
        if (sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE) == 1 &&
            sigismember(&old_set_, SIGPIPE) == 0) {
            const timespec zero{};
            (void)sigtimedwait(&pipe_set_, nullptr, &zero);
        }
        (void)pthread_sigmask(SIG_SETMASK, &old_set_, nullptr);
    }
    BlockSigpipe(const BlockSigpipe&) = delete;
    BlockSigpipe& operator=(const BlockSigpipe&) = delete;

    [[nodiscard]] bool active() const { return active_; }

private:
    sigset_t pipe_set_{};
    sigset_t old_set_{};
    bool active_ = false;
};

/// Acciones de posix_spawn y atributos, destruidos al salir.
struct SpawnSetup {
    posix_spawn_file_actions_t actions{};
    posix_spawnattr_t attributes{};
    bool have_actions = false;
    bool have_attributes = false;
    SpawnSetup() {
        have_actions = posix_spawn_file_actions_init(&actions) == 0;
        have_attributes = posix_spawnattr_init(&attributes) == 0;
    }
    ~SpawnSetup() {
        if (have_actions) {
            posix_spawn_file_actions_destroy(&actions);
        }
        if (have_attributes) {
            posix_spawnattr_destroy(&attributes);
        }
    }
    SpawnSetup(const SpawnSetup&) = delete;
    SpawnSetup& operator=(const SpawnSetup&) = delete;
};

/// Gracia entre SIGTERM y SIGKILL al cancelar.
constexpr std::chrono::milliseconds kTerminateGrace{200};

/// SIGTERM, hasta kTerminateGrace para que salga y si no, SIGKILL.
void terminate(pid_t pid, int& status) {
    ::kill(pid, SIGTERM);
    const Clock::time_point give_up = Clock::now() + kTerminateGrace;
    while (Clock::now() < give_up) {
        const pid_t done = ::waitpid(pid, &status, WNOHANG);
        if (done == pid || (done < 0 && errno != EINTR)) {
            return;
        }
        ::poll(nullptr, 0, 10);
    }
    ::kill(pid, SIGKILL);
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
}

} // namespace

ProcessResult run_process(const std::vector<std::string>& argv, const ProcessOptions& options) {
    ProcessResult result;
    if (argv.empty() || argv.front().empty()) {
        return result;
    }
    const Clock::time_point deadline = Clock::now() + options.timeout;
    const BlockSigpipe no_sigpipe;
    if (!no_sigpipe.active()) {
        return result;
    }

    Fd in_read;
    Fd in_write;
    Fd out_read;
    Fd out_write;
    Fd err_read;
    Fd err_write;
    const bool has_input = !options.input.empty();
    if ((has_input && !make_pipe(in_read, in_write)) ||
        (options.capture_output &&
         (!make_pipe(out_read, out_write) || !make_pipe(err_read, err_write)))) {
        return result;
    }

    SpawnSetup setup;
    bool ready = setup.have_actions && setup.have_attributes;
    if (ready) {
        posix_spawn_file_actions_t* actions = &setup.actions;
        ready = (has_input
                     ? posix_spawn_file_actions_adddup2(actions, in_read.get(), STDIN_FILENO)
                     : posix_spawn_file_actions_addopen(actions, STDIN_FILENO, "/dev/null",
                                                        O_RDONLY, 0)) == 0;
        if (options.capture_output) {
            ready = ready &&
                    posix_spawn_file_actions_adddup2(actions, out_write.get(), STDOUT_FILENO) ==
                        0 &&
                    posix_spawn_file_actions_adddup2(actions, err_write.get(), STDERR_FILENO) == 0;
        } else {
            // No ensucian la pantalla de FTXUI.
            ready = ready &&
                    posix_spawn_file_actions_addopen(actions, STDOUT_FILENO, "/dev/null",
                                                     O_WRONLY, 0) == 0 &&
                    posix_spawn_file_actions_addopen(actions, STDERR_FILENO, "/dev/null",
                                                     O_WRONLY, 0) == 0;
        }
    }
    // El hijo empieza sin señales bloqueadas y con SIGPIPE en su acción por
    // defecto, sin importar lo que tenga este hilo.
    if (ready) {
        sigset_t none;
        sigemptyset(&none);
        sigset_t defaults;
        sigemptyset(&defaults);
        sigaddset(&defaults, SIGPIPE);
        ready = posix_spawnattr_setsigmask(&setup.attributes, &none) == 0 &&
                posix_spawnattr_setsigdefault(&setup.attributes, &defaults) == 0 &&
                posix_spawnattr_setflags(&setup.attributes,
                                         static_cast<short>(POSIX_SPAWN_SETSIGMASK |
                                                            POSIX_SPAWN_SETSIGDEF)) == 0;
    }
    if (!ready) {
        return result;
    }
    std::vector<std::string> args = argv;
    std::vector<char*> c_args;
    c_args.reserve(args.size() + 1);
    for (std::string& arg : args) {
        c_args.push_back(arg.data());
    }
    c_args.push_back(nullptr);
    pid_t pid = -1;
    const int spawned =
        posix_spawnp(&pid, c_args.front(), &setup.actions, &setup.attributes, c_args.data(),
                     environ);
    if (spawned != 0) {
        result.status = spawned == ENOENT ? ProcessResult::Status::NotFound
                                          : ProcessResult::Status::Failed;
        return result;
    }
    // Las puntas del hijo: si se quedan abiertas aquí, nunca llega el EOF.
    in_read.reset();
    out_write.reset();
    err_write.reset();
    if ((in_write.open() && !set_nonblocking(in_write.get())) ||
        (out_read.open() && !set_nonblocking(out_read.get())) ||
        (err_read.open() && !set_nonblocking(err_read.get()))) {
        ::kill(pid, SIGKILL);
    }

    std::string_view input = options.input;
    result.input_written = !has_input;
    int status = 0;
    bool exited = false;
    bool timed_out = false;
    bool cancelled = false;
    while (true) {
        const pid_t done = ::waitpid(pid, &status, WNOHANG);
        if (done == pid) {
            exited = true;
            break;
        }
        if (done < 0 && errno != EINTR) {
            break;
        }
        if (Clock::now() >= deadline) {
            ::kill(pid, SIGKILL);
            timed_out = true;
            while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
            }
            break;
        }
        if (options.cancel != nullptr && options.cancel->is_cancelled()) {
            terminate(pid, status);
            cancelled = true;
            break;
        }
        pollfd waiting[3] = {};
        nfds_t count = 0;
        if (in_write.open()) {
            waiting[count++] = pollfd{in_write.get(), POLLOUT, 0};
        }
        if (out_read.open()) {
            waiting[count++] = pollfd{out_read.get(), POLLIN, 0};
        }
        if (err_read.open()) {
            waiting[count++] = pollfd{err_read.get(), POLLIN, 0};
        }
        // Despierta seguido para ver si el hijo ya salió (no hay un
        // descriptor para eso sin pidfd).
        const int wait_ms = std::min(remaining_ms(deadline), 20);
        if (count == 0) {
            ::poll(nullptr, 0, wait_ms);
            continue;
        }
        if (::poll(waiting, count, wait_ms) < 0 && errno != EINTR) {
            break;
        }
        if (in_write.open()) {
            bool closed = false; // EPIPE u otro error: el hijo ya no lee.
            while (!input.empty()) {
                const ssize_t written = ::write(in_write.get(), input.data(), input.size());
                if (written > 0) {
                    input.remove_prefix(static_cast<std::size_t>(written));
                    continue;
                }
                if (written < 0 && errno == EINTR) {
                    continue;
                }
                closed = !(written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
                break;
            }
            if (input.empty() || closed) {
                result.input_written = input.empty();
                in_write.reset(); // EOF para el hijo.
            }
        }
        read_available(out_read, result.out);
        read_available(err_read, result.err);
    }
    // Lo que quedó en los pipes antes de salir.
    read_available(out_read, result.out);
    read_available(err_read, result.err);
    if (cancelled) {
        result.status = ProcessResult::Status::Cancelled;
    } else if (timed_out) {
        result.status = ProcessResult::Status::TimedOut;
    } else if (exited && WIFEXITED(status)) {
        result.status = ProcessResult::Status::Exited;
        result.exit_code = WEXITSTATUS(status);
    } else if (exited && WIFSIGNALED(status)) {
        result.status = ProcessResult::Status::Signaled;
    }
    return result;
}

bool is_executable_file(const std::string& path) {
    struct stat info{};
    return ::stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode) &&
           ::access(path.c_str(), X_OK) == 0;
}

std::string exec_program(const std::string& path, const std::vector<std::string>& argv) {
    std::vector<std::string> args = argv;
    std::vector<char*> c_args;
    c_args.reserve(args.size() + 1);
    for (std::string& arg : args) {
        c_args.push_back(arg.data());
    }
    c_args.push_back(nullptr);
    ::execv(path.c_str(), c_args.data());
    return std::strerror(errno);
}

std::optional<std::string> find_in_path(std::string_view program) {
    const char* path = std::getenv("PATH");
    if (path == nullptr || program.empty() || program.find('/') != std::string_view::npos) {
        return std::nullopt;
    }
    std::string_view rest = path;
    while (true) {
        const std::size_t end = rest.find(':');
        const std::string_view dir = rest.substr(0, end);
        // Una carpeta vacía sería la actual: no se usa.
        if (!dir.empty()) {
            std::string candidate = std::string{dir} + "/" + std::string{program};
            if (is_executable_file(candidate)) {
                return candidate;
            }
        }
        if (end == std::string_view::npos) {
            return std::nullopt;
        }
        rest.remove_prefix(end + 1);
    }
}

std::optional<unsigned long> current_user_id() {
    return static_cast<unsigned long>(::getuid());
}

} // namespace chatbot::cli
