// chatbot-rpc de verdad, con pipes (solo POSIX): run_process le escribe por
// stdin y lee su stdout. Con HOME, XDG_* y CHAT_* propios y sin
// configuración, así que nunca toca la red ni la configuración del usuario.

#include "child_process.h"

#include "temp_dir.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

namespace {

/// El ejecutable con un entorno limpio (env -i) dentro de dir.
std::vector<std::string> rpc_command(const chatbot_test::ScopedTempDir& dir,
                                     const std::string& debug_log = {}) {
    std::vector<std::string> argv{"env", "-i", "HOME=" + dir.string(),
                                  "XDG_CONFIG_HOME=" + (dir.path() / "config").string(),
                                  "XDG_DATA_HOME=" + (dir.path() / "data").string(),
                                  "PATH=/usr/bin:/bin"};
    // Los sanitizadores siguen con sus opciones.
    for (const char* name : {"ASAN_OPTIONS", "UBSAN_OPTIONS", "TSAN_OPTIONS", "LSAN_OPTIONS"}) {
        if (const char* value = std::getenv(name)) {
            argv.push_back(std::string{name} + "=" + value);
        }
    }
    if (!debug_log.empty()) {
        argv.push_back("CHAT_DEBUG_RPC=" + debug_log);
    }
    argv.emplace_back(CHATBOT_RPC_EXECUTABLE);
    return argv;
}

/// Cada línea de stdout, interpretada; falla si alguna no es JSON-RPC 2.0.
std::vector<nlohmann::json> messages_of(const std::string& out) {
    std::vector<nlohmann::json> messages;
    std::istringstream lines{out};
    std::string line;
    while (std::getline(lines, line)) {
        INFO(line);
        const nlohmann::json message = nlohmann::json::parse(line, nullptr, false);
        REQUIRE_FALSE(message.is_discarded());
        CHECK(message.value("jsonrpc", "") == "2.0");
        messages.push_back(message);
    }
    CHECK((out.empty() || out.back() == '\n'));
    return messages;
}

constexpr const char* kInitialize =
    R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocol_version":1,)"
    R"("client":{"name":"prueba","version":"0"}}})"
    "\n";

} // namespace

TEST_CASE("chatbot-rpc sin configuración: initialize, chat.send, shutdown y exit",
          "[rpc][proceso]") {
    const chatbot_test::ScopedTempDir dir;
    const std::string log = (dir.path() / "rpc.log").string();
    const std::string input =
        std::string{kInitialize} +
        R"({"jsonrpc":"2.0","id":2,"method":"chat.send","params":{"conversation":"c1","text":"hola"}})"
        "\n"
        R"({"jsonrpc":"2.0","id":3,"method":"shutdown","params":{}})"
        "\n"
        R"({"jsonrpc":"2.0","method":"exit"})"
        "\n";
    chatbot::cli::ProcessOptions options;
    options.input = input;
    options.timeout = std::chrono::seconds{20};
    const chatbot::cli::ProcessResult result =
        chatbot::cli::run_process(rpc_command(dir, log), options);
    INFO(result.err);
    REQUIRE(result.status == chatbot::cli::ProcessResult::Status::Exited);
    CHECK(result.exit_code == 0);
    const std::vector<nlohmann::json> messages = messages_of(result.out);
    REQUIRE(messages.size() == 4);
    CHECK(messages[0]["id"] == 1);
    CHECK(messages[0]["result"]["ready"] == false);
    CHECK(messages[0]["result"]["server"]["version"] == "0.1.0");
    CHECK(messages[1]["id"] == 2);
    CHECK(messages[1]["error"]["code"] == -32004);
    CHECK(messages[2]["method"] == "conversation.closed");
    CHECK(messages[3]["id"] == 3);
    CHECK(messages[3].contains("result"));
    // Con pipes, stdin no es una terminal: sin el aviso del doble clic.
    CHECK(result.err.find("es para otras interfaces") == std::string::npos);

    // CHAT_DEBUG_RPC: lo que entró y lo que salió.
    std::ifstream file{log};
    const std::string written{std::istreambuf_iterator<char>{file}, {}};
    CHECK(written.find("→ {\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\"") !=
          std::string::npos);
    CHECK(written.find("← {\"jsonrpc\":\"2.0\",\"id\":3,\"result\":{}}") != std::string::npos);
}

TEST_CASE("chatbot-rpc: EOF sale con 0 en menos de 3 s", "[rpc][proceso]") {
    const chatbot_test::ScopedTempDir dir;
    chatbot::cli::ProcessOptions options;
    options.input = kInitialize;
    options.timeout = std::chrono::seconds{20};
    const auto begin = std::chrono::steady_clock::now();
    const chatbot::cli::ProcessResult result = chatbot::cli::run_process(rpc_command(dir), options);
    const auto elapsed = std::chrono::steady_clock::now() - begin;
    INFO(result.err);
    REQUIRE(result.status == chatbot::cli::ProcessResult::Status::Exited);
    CHECK(result.exit_code == 0);
    CHECK(elapsed < std::chrono::seconds{3});
    const std::vector<nlohmann::json> messages = messages_of(result.out);
    REQUIRE(messages.size() == 1);
    CHECK(messages[0]["result"]["conversation"] == "c1");
}

TEST_CASE("chatbot-rpc: exit sin shutdown sale con 1", "[rpc][proceso]") {
    const chatbot_test::ScopedTempDir dir;
    chatbot::cli::ProcessOptions options;
    const std::string input = std::string{kInitialize} + R"({"jsonrpc":"2.0","method":"exit"})" "\n";
    options.input = input;
    options.timeout = std::chrono::seconds{20};
    const chatbot::cli::ProcessResult result = chatbot::cli::run_process(rpc_command(dir), options);
    INFO(result.err);
    REQUIRE(result.status == chatbot::cli::ProcessResult::Status::Exited);
    CHECK(result.exit_code == 1);
}
