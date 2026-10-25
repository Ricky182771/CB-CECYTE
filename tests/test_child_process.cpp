// Programas externos (child_process.h): salida capturada, stdin, tope de tiempo y
// búsqueda en PATH. Lanzan /bin/sh y programas de coreutils; solo POSIX
// (en Windows run_process no lanza nada).

#include "child_process.h"

#include "chatbot/platform.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

using chatbot::cli::find_in_path;
using chatbot::cli::ProcessOptions;
using chatbot::cli::ProcessResult;
using chatbot::cli::run_process;
using namespace std::chrono_literals;

TEST_CASE("run_process: en Windows no lanza nada", "[process]") {
    if (chatbot::current_os() != chatbot::Os::Windows) {
        SKIP("solo Windows");
    }
    CHECK(run_process({"cmd", "/c", "echo"}).status == ProcessResult::Status::Failed);
    CHECK_FALSE(find_in_path("cmd").has_value());
}

#ifndef _WIN32

TEST_CASE("run_process: stdout, stderr y código de salida", "[process]") {
    const ProcessResult result =
        run_process({"/bin/sh", "-c", "printf 'hola\\nadios'; printf 'mal' >&2; exit 3"});
    CHECK(result.status == ProcessResult::Status::Exited);
    CHECK(result.exit_code == 3);
    CHECK_FALSE(result.ok());
    CHECK(result.out == "hola\nadios");
    CHECK(result.err == "mal");

    const ProcessResult fine = run_process({"true"});
    CHECK(fine.ok());
    CHECK(fine.out.empty());
}

TEST_CASE("run_process: los argumentos van tal cual, sin shell", "[process]") {
    const ProcessResult result = run_process({"printf", "%s|", "a b", "$HOME", "*", "'q'"});
    REQUIRE(result.ok());
    CHECK(result.out == "a b|$HOME|*|'q'|");
}

TEST_CASE("run_process: el texto llega por stdin", "[process]") {
    ProcessOptions options;
    const std::string big(300 * 1024, 'x'); // Más que el búfer de un pipe.
    options.input = big;
    const ProcessResult result = run_process({"wc", "-c"}, options);
    REQUIRE(result.ok());
    CHECK(result.input_written);
    CHECK(std::stoul(result.out) == big.size());

    // Un hijo que no lee stdin: no tumba a la app (SIGPIPE) y lo dice.
    const ProcessResult ignored = run_process({"true"}, options);
    CHECK(ignored.status == ProcessResult::Status::Exited);
    CHECK_FALSE(ignored.input_written);
}

TEST_CASE("run_process: sin capturar, la salida va a /dev/null", "[process]") {
    ProcessOptions options;
    options.capture_output = false;
    const ProcessResult result = run_process({"/bin/sh", "-c", "echo hola; echo mal >&2"}, options);
    CHECK(result.ok());
    CHECK(result.out.empty());
    CHECK(result.err.empty());
}

TEST_CASE("run_process: un programa que no termina se mata al pasar el tope", "[process]") {
    ProcessOptions options;
    options.timeout = 300ms;
    const auto started = std::chrono::steady_clock::now();
    const ProcessResult result = run_process({"sleep", "30"}, options);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    CHECK(result.status == ProcessResult::Status::TimedOut);
    CHECK(elapsed >= 300ms);
    CHECK(elapsed < 5s);
}

TEST_CASE("run_process: al cancelar se mata el programa", "[process]") {
    // El segundo ignora SIGTERM: le llega SIGKILL tras la gracia.
    for (const std::vector<std::string>& argv :
         {std::vector<std::string>{"sleep", "60"},
          std::vector<std::string>{"/bin/sh", "-c", "trap '' TERM; while :; do :; done"}}) {
        INFO(argv.back());
        chatbot::CancelToken cancel;
        ProcessOptions options;
        options.timeout = 30s;
        options.cancel = &cancel;
        std::thread canceller{[&cancel] {
            std::this_thread::sleep_for(100ms);
            cancel.cancel();
        }};
        const auto started = std::chrono::steady_clock::now();
        const ProcessResult result = run_process(argv, options);
        const auto elapsed = std::chrono::steady_clock::now() - started;
        canceller.join();
        const double ms = std::chrono::duration<double, std::milli>(elapsed).count();
        WARN("cancelar run_process: " << ms << " ms");
        CHECK(result.status == ProcessResult::Status::Cancelled);
        CHECK_FALSE(result.ok());
        CHECK(elapsed < 1s);
    }
}

TEST_CASE("run_process: regresa cuando el hijo sale aunque un nieto tenga la salida",
          "[process]") {
    ProcessOptions options;
    options.timeout = 10s;
    const auto started = std::chrono::steady_clock::now();
    const ProcessResult result = run_process({"/bin/sh", "-c", "sleep 3 & echo listo"}, options);
    CHECK(result.ok());
    CHECK(result.out == "listo\n");
    CHECK(std::chrono::steady_clock::now() - started < 2s);
}

TEST_CASE("run_process: no existe el programa", "[process]") {
    CHECK(run_process({"programa-que-no-existe-chatbot"}).status ==
          ProcessResult::Status::NotFound);
    CHECK(run_process({"/no/existe/x"}).status == ProcessResult::Status::NotFound);
    CHECK(run_process({}).status == ProcessResult::Status::Failed);
}

TEST_CASE("run_process: guarda hasta kMaxProcessOutput", "[process]") {
    const ProcessResult result =
        run_process({"head", "-c", std::to_string(chatbot::cli::kMaxProcessOutput + 5000),
                     "/dev/zero"});
    CHECK(result.ok());
    CHECK(result.out.size() == chatbot::cli::kMaxProcessOutput);
}

TEST_CASE("find_in_path: la ruta completa del programa", "[process]") {
    const auto sh = find_in_path("sh");
    REQUIRE(sh.has_value());
    CHECK(std::filesystem::path{*sh}.is_absolute());
    CHECK(std::filesystem::path{*sh}.filename() == "sh");
    CHECK_FALSE(find_in_path("programa-que-no-existe-chatbot").has_value());
    CHECK_FALSE(find_in_path("bin/sh").has_value()); // Con '/' no se busca.
    CHECK_FALSE(find_in_path("").has_value());
}

#endif
