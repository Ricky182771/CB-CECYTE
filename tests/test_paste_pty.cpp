// Pegado entre corchetes por una pseudoterminal, solo POSIX: arranca
// chatbot_paste_probe (App mínimo con BracketedPaste, como main.cpp) con la
// esclava como terminal de control y le escribe como una terminal real.
//
// - Regresión del parche de FTXUI (cmake/patches/ftxui-7.0.3-drain-input.patch,
//   el #1351 de upstream): 200 KB. Sin el parche, FTXUI v7.0.3 lee 128 bytes
//   por vuelta a 60 vueltas por segundo y el pegado no puede entrar en menos
//   de 26 s, sea cual sea el equipo; con el parche, en una fracción de segundo.
//   Además deben llegar completos: con el parche, FTXUI parsea hasta 64 KiB
//   antes del primer evento (770 ms en asan) y la salida de emergencia de
//   BracketedPaste no debe tomar esa espera por una pausa.
// - La salida de emergencia de verdad: sin 201~ y con la terminal quieta, la
//   app sigue dibujando cuadros (RequestAnimationFrame) y la siguiente tecla
//   cierra el pegado.

#include "paste.h"
#include "performance.hpp"
#include "temp_dir.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

extern char** environ;

namespace {

using Clock = std::chrono::steady_clock;

/// Lo que dibuja chatbot_paste_probe desde el primer cuadro (ya en modo crudo).
constexpr std::string_view kReadyText = "paste-probe listo";
/// Esperas máximas: verificaciones funcionales, en todos los presets. La del
/// pegado grande deja pasar los 26 s que tarda sin el parche (el límite de
/// rendimiento es aparte, con CHATBOT_STRICT_PERF).
constexpr auto kReadyTimeout = std::chrono::seconds{20};
constexpr auto kPasteTimeout = std::chrono::seconds{90};
constexpr auto kShortTimeout = std::chrono::seconds{20};

/// Entorno del hijo: el del proceso con TERM=xterm-256color.
std::vector<std::string> child_environment() {
    std::vector<std::string> env;
    for (char** entry = environ; *entry != nullptr; ++entry) {
        if (std::string_view{*entry}.substr(0, 5) != "TERM=") {
            env.emplace_back(*entry);
        }
    }
    env.emplace_back("TERM=xterm-256color");
    return env;
}

/// Descriptor que se cierra solo (también si falla un REQUIRE del constructor
/// de ProbeSession).
struct OwnedFd {
    int fd = -1;
    OwnedFd() = default;
    ~OwnedFd() {
        if (fd >= 0) {
            ::close(fd);
        }
    }
    OwnedFd(const OwnedFd&) = delete;
    OwnedFd& operator=(const OwnedFd&) = delete;
};

/// chatbot_paste_probe en una pseudoterminal nueva de 80×24. Mientras se
/// escribe o se espera, siempre vacía lo que dibuja: si no, el hijo se
/// bloquea al escribir y deja de leer. Si sigue vivo al final, se mata.
class ProbeSession {
public:
    explicit ProbeSession(const std::string& result_path) {
        master_.fd = ::posix_openpt(O_RDWR | O_NOCTTY);
        const int master = master_.fd;
        REQUIRE(master >= 0);
        REQUIRE(::grantpt(master) == 0);
        REQUIRE(::unlockpt(master) == 0);
        const char* slave_name = ::ptsname(master);
        REQUIRE(slave_name != nullptr);
        const std::string slave = slave_name;
        winsize size{};
        size.ws_row = 24;
        size.ws_col = 80;
        REQUIRE(::ioctl(master, TIOCSWINSZ, &size) == 0);
        REQUIRE(::fcntl(master, F_SETFL, ::fcntl(master, F_GETFL) | O_NONBLOCK) == 0);

        // Todo lo que usa el hijo se arma antes del fork: después solo
        // llamadas seguras (setsid, open, ioctl, dup2, close, execve).
        const std::string probe = CHATBOT_PASTE_PROBE;
        const std::vector<std::string> env = child_environment();
        std::vector<char*> envp;
        for (const std::string& entry : env) {
            envp.push_back(const_cast<char*>(entry.c_str()));
        }
        envp.push_back(nullptr);
        std::vector<char*> argv{const_cast<char*>(probe.c_str()),
                                const_cast<char*>(result_path.c_str()), nullptr};

        pid_ = ::fork();
        if (pid_ == 0) {
            // Sesión nueva con la esclava como terminal de control (FTXUI usa
            // stdin y stdout, y sus consultas de capacidades van a la terminal).
            ::setsid();
            const int fd = ::open(slave.c_str(), O_RDWR);
            if (fd < 0) {
                ::_exit(126);
            }
            ::ioctl(fd, TIOCSCTTY, 0);
            ::dup2(fd, STDIN_FILENO);
            ::dup2(fd, STDOUT_FILENO);
            ::dup2(fd, STDERR_FILENO);
            if (fd > STDERR_FILENO) {
                ::close(fd);
            }
            ::close(master);
            ::execve(argv[0], argv.data(), envp.data());
            ::_exit(127);
        }
        REQUIRE(pid_ > 0);
    }

    ~ProbeSession() {
        if (pid_ > 0 && !exited_) {
            ::kill(pid_, SIGKILL);
            ::waitpid(pid_, &status_, 0);
        }
    }
    ProbeSession(const ProbeSession&) = delete;
    ProbeSession& operator=(const ProbeSession&) = delete;

    /// Espera el primer cuadro: entonces la terminal ya está en modo crudo.
    [[nodiscard]] bool wait_ready() {
        const auto deadline = Clock::now() + kReadyTimeout;
        while (!ready_ && Clock::now() < deadline && !exited()) {
            pump(POLLIN, 50);
        }
        return ready_;
    }

    /// Escribe data; para antes si el hijo sale o pasa deadline. Devuelve los
    /// bytes escritos.
    std::size_t write(std::string_view data, Clock::time_point deadline) {
        std::size_t written = 0;
        while (written < data.size() && !exited() && Clock::now() < deadline) {
            pump(POLLIN | POLLOUT, 10);
            const ssize_t put = ::write(master_.fd, data.data() + written, data.size() - written);
            if (put > 0) {
                written += static_cast<std::size_t>(put);
            } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                FAIL("no se pudo escribir en la pty: errno " << errno);
            }
        }
        return written;
    }

    /// Deja pasar el tiempo sin escribir nada (el hijo sigue dibujando).
    void idle(Clock::duration duration) {
        const auto until = Clock::now() + duration;
        while (Clock::now() < until && !exited()) {
            pump(POLLIN, 10);
        }
    }

    /// Espera a que el hijo salga; true si salió con 0 antes de deadline.
    [[nodiscard]] bool wait_exit(Clock::time_point deadline) {
        while (!exited() && Clock::now() < deadline) {
            pump(POLLIN, 10);
        }
        return exited_ && WIFEXITED(status_) && WEXITSTATUS(status_) == 0;
    }

private:
    bool exited() {
        if (!exited_ && ::waitpid(pid_, &status_, WNOHANG) == pid_) {
            exited_ = true;
        }
        return exited_;
    }

    /// Espera hasta timeout_ms a la pty y lee todo lo que haya dibujado el
    /// hijo; recent_ guarda la cola, para encontrar kReadyText aunque llegue
    /// partido entre dos lecturas.
    void pump(short events, int timeout_ms) {
        pollfd pfd{master_.fd, events, 0};
        ::poll(&pfd, 1, timeout_ms);
        char buffer[4096];
        for (;;) {
            const ssize_t got = ::read(master_.fd, buffer, sizeof buffer);
            if (got <= 0) {
                return;
            }
            recent_.append(buffer, static_cast<std::size_t>(got));
            ready_ = ready_ || recent_.find(kReadyText) != std::string::npos;
            if (recent_.size() > kReadyText.size()) {
                recent_.erase(0, recent_.size() - kReadyText.size());
            }
        }
    }

    OwnedFd master_;
    pid_t pid_ = -1;
    bool exited_ = false;
    int status_ = 0;
    bool ready_ = false;
    std::string recent_;
};

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

/// "lorem ipsum…" en líneas de 80 bytes terminadas en "\r" (lo que manda
/// una terminal por cada salto), size bytes exactos.
std::string paste_body(std::size_t size) {
    static constexpr std::string_view kWords = "lorem ipsum dolor sit amet ";
    std::string body;
    body.reserve(size);
    while (body.size() < size) {
        body += (body.size() % 80 == 79) ? '\r' : kWords[body.size() % kWords.size()];
    }
    return body;
}

} // namespace

TEST_CASE("pegado entre corchetes de 200 KB por una pseudoterminal", "[pegado][pty][rendimiento]") {
    const chatbot_test::ScopedTempDir dir;
    const std::string result_path = (dir.path() / "pegado.txt").string();
    ProbeSession probe{result_path};
    REQUIRE(probe.wait_ready());

    const std::string body = paste_body(200'000);
    const std::string payload = "\x1B[200~" + body + "\x1B[201~";
    const auto start = Clock::now();
    const auto deadline = start + kPasteTimeout;
    const std::size_t written = probe.write(payload, deadline);
    const bool exited = probe.wait_exit(deadline);
    const double seconds = std::chrono::duration<double>(Clock::now() - start).count();
    INFO("bytes escritos: " << written << " de " << payload.size() << "; " << seconds << " s");
    REQUIRE(exited);

    // Completo y sin cambios ("\r" → "\n", como en la app), en un solo pegado.
    const std::string received = read_file(result_path);
    std::string expected = body;
    for (char& c : expected) {
        if (c == '\r') {
            c = '\n';
        }
    }
    // Sin comparar las cadenas con CHECK: Catch2 imprimiría 200 KB.
    std::size_t first_difference = 0;
    while (first_difference < received.size() && first_difference < expected.size() &&
           received[first_difference] == expected[first_difference]) {
        ++first_difference;
    }
    INFO("primera diferencia en el byte " << first_difference);
    CHECK(written == payload.size());
    CHECK(received.size() == expected.size());
    CHECK(first_difference == expected.size());

    WARN("200 KB pegados por una pty: " << seconds << " s");
    if (chatbot_test::strict_performance()) {
        CHECK(seconds < 2.0);
    }
}

TEST_CASE("pegado sin 201~ por una pseudoterminal: tras una pausa, la siguiente tecla lo cierra",
          "[pegado][pty]") {
    const chatbot_test::ScopedTempDir dir;
    const std::string result_path = (dir.path() / "pegado.txt").string();
    ProbeSession probe{result_path};
    REQUIRE(probe.wait_ready());

    const auto deadline = Clock::now() + kShortTimeout;
    const std::string_view opened = "\x1B[200~hola\rmundo";
    REQUIRE(probe.write(opened, deadline) == opened.size());
    // Más que kPasteIdleLimit sin escribir: la app tiene que seguir dibujando
    // (si no, BracketedPaste no ve la pausa y la tecla se pegaría también).
    probe.idle(chatbot::cli::kPasteIdleLimit * 3);
    REQUIRE(probe.write("x", deadline) == 1);
    REQUIRE(probe.wait_exit(deadline));
    CHECK(read_file(result_path) == "hola\nmundo");
}
