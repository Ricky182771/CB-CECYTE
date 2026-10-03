#include "request_runner.h"

#include "blocking_transport.hpp"
#include "chatbot/chat_client.h"
#include "chatbot/sleeper.h"
#include "fake_transport.hpp"

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

using chatbot::ErrorKind;
using chatbot::HttpResponse;
using chatbot::Message;
using chatbot::Role;
using chatbot::cli::RequestRunner;
using chatbot_test::BlockingTransport;
using chatbot_test::kThreadTimeout;
using chatbot_test::TaskQueue;
using Clock = std::chrono::steady_clock;

/// Resultado de una petición del runner, visto desde el "hilo de la interfaz".
struct Outcome {
    std::vector<std::string> deltas;
    std::optional<chatbot::Result<chatbot::CompletionInfo>> result;
    std::size_t dropped = 0;
    int done_calls = 0;
    bool off_thread_callback = false; ///< Algún callback corrió fuera del hilo de la prueba.
};

/// Arnés: cliente con el transporte dado, cola de tareas y runner.
struct RunnerHarness {
    TaskQueue queue;
    std::unique_ptr<chatbot::ChatClient> client;
    std::unique_ptr<RequestRunner> runner;
    Outcome outcome;
    std::thread::id ui_thread = std::this_thread::get_id();

    RunnerHarness(std::unique_ptr<chatbot::Transport> transport,
                  std::unique_ptr<chatbot::Sleeper> sleeper, std::size_t limit = 0) {
        client = std::make_unique<chatbot::ChatClient>(chatbot_test::test_config(),
                                                       std::move(transport), std::move(sleeper));
        runner = std::make_unique<RequestRunner>(
            *client, limit, [this](RequestRunner::Task task) { queue.post(std::move(task)); });
    }

    bool start(std::vector<Message> history = chatbot_test::sample_messages()) {
        return runner->start(
            std::move(history),
            [this](std::string delta) {
                outcome.off_thread_callback |= std::this_thread::get_id() != ui_thread;
                outcome.deltas.push_back(std::move(delta));
            },
            [this](chatbot::Result<chatbot::CompletionInfo> result, std::size_t dropped) {
                outcome.off_thread_callback |= std::this_thread::get_id() != ui_thread;
                outcome.result = std::move(result);
                outcome.dropped = dropped;
                ++outcome.done_calls;
            });
    }

    /// Vacía la cola hasta que llegue on_done (con límite de tiempo).
    bool wait_done() {
        return queue.run_until([this] { return outcome.done_calls > 0; });
    }
};

std::string sse_flow(const std::vector<std::string>& pieces) {
    std::string flow;
    for (const std::string& piece : pieces) {
        flow += "data: {\"choices\":[{\"delta\":{\"content\":\"" + piece + "\"}}]}\n\n";
    }
    return flow + "data: [DONE]\n\n";
}

/// Historial con una vuelta vieja grande (600 B) y el mensaje actual.
std::vector<Message> history_with_old_turn() {
    return {
        Message{Role::System, "sistema"},
        Message{Role::User, std::string(300, 'u')},
        Message{Role::Assistant, std::string(300, 'a')},
        Message{Role::User, "pregunta actual"},
    };
}

std::size_t sent_messages(const chatbot::HttpRequest& request) {
    return nlohmann::json::parse(request.body).at("messages").size();
}

} // namespace

TEST_CASE("RequestRunner: deltas completos y en orden por la cola, on_done una vez",
          "[runner][hilos]") {
    auto transport = std::make_unique<chatbot_test::FakeTransport>();
    transport->chunk_size = 5;
    transport->responses.push_back(
        HttpResponse{200, sse_flow({"Hola", ", ", "¿qué ", "tal?"}), std::nullopt, false, ""});
    RunnerHarness harness(std::move(transport), std::make_unique<chatbot_test::FakeSleeper>());

    REQUIRE(harness.start());
    CHECK(harness.runner->busy());
    REQUIRE(harness.wait_done());

    std::string joined;
    for (const std::string& delta : harness.outcome.deltas) {
        joined += delta;
    }
    CHECK(joined == "Hola, ¿qué tal?");
    CHECK(harness.outcome.deltas.size() == 4);
    REQUIRE(harness.outcome.result.has_value());
    CHECK(harness.outcome.result->is_ok());
    CHECK(harness.outcome.dropped == 0);
    CHECK_FALSE(harness.outcome.off_thread_callback);
    CHECK_FALSE(harness.runner->busy());

    // Nada más llega después de on_done.
    CHECK_FALSE(harness.queue.run_until([] { return false; }, std::chrono::milliseconds{50}));
    CHECK(harness.outcome.done_calls == 1);
}

TEST_CASE("RequestRunner: start mientras está ocupado devuelve false", "[runner][hilos]") {
    auto transport = std::make_unique<BlockingTransport>(BlockingTransport::Mode::Block);
    BlockingTransport* blocking = transport.get();
    RunnerHarness harness(std::move(transport), std::make_unique<chatbot_test::FakeSleeper>());

    REQUIRE(harness.start());
    REQUIRE(blocking->started.wait_for(kThreadTimeout));
    CHECK_FALSE(harness.start());
    CHECK(harness.runner->busy());

    harness.runner->cancel();
    REQUIRE(harness.wait_done());
    CHECK(blocking->requests().size() == 1);
    CHECK(harness.outcome.done_calls == 1);
    CHECK_FALSE(harness.runner->busy());
}

TEST_CASE("RequestRunner: cancelar sin datos termina en menos de 2 s", "[runner][hilos]") {
    auto transport = std::make_unique<BlockingTransport>(BlockingTransport::Mode::Block);
    BlockingTransport* blocking = transport.get();
    RunnerHarness harness(std::move(transport), std::make_unique<chatbot_test::FakeSleeper>());

    REQUIRE(harness.start());
    REQUIRE(blocking->started.wait_for(kThreadTimeout));
    const auto cancelled_at = Clock::now();
    harness.runner->cancel();
    REQUIRE(harness.wait_done());

    CHECK(Clock::now() - cancelled_at < std::chrono::seconds{2});
    REQUIRE(harness.outcome.result.has_value());
    REQUIRE(harness.outcome.result->is_error());
    CHECK(harness.outcome.result->error().kind == ErrorKind::Cancelled);
}

TEST_CASE("RequestRunner: cancelar a media respuesta conserva los deltas ya entregados",
          "[runner][hilos]") {
    auto transport = std::make_unique<BlockingTransport>(BlockingTransport::Mode::Block);
    transport->first_chunk = "data: {\"choices\":[{\"delta\":{\"content\":\"parcial\"}}]}\n\n";
    BlockingTransport* blocking = transport.get();
    RunnerHarness harness(std::move(transport), std::make_unique<chatbot_test::FakeSleeper>());

    REQUIRE(harness.start());
    REQUIRE(blocking->started.wait_for(kThreadTimeout));
    REQUIRE(harness.queue.run_until([&] { return !harness.outcome.deltas.empty(); }));
    harness.runner->cancel();
    REQUIRE(harness.wait_done());

    CHECK(harness.outcome.deltas == std::vector<std::string>{"parcial"});
    REQUIRE(harness.outcome.result->is_error());
    CHECK(harness.outcome.result->error().kind == ErrorKind::Cancelled);
}

TEST_CASE("RequestRunner: cancelar durante la espera de reintento con RealSleeper",
          "[runner][hilos]") {
    auto transport = std::make_unique<BlockingTransport>(
        BlockingTransport::Mode::Respond, HttpResponse{429, "{}", std::string{"60"}, false, ""});
    BlockingTransport* blocking = transport.get();
    RunnerHarness harness(std::move(transport), std::make_unique<chatbot::RealSleeper>());

    REQUIRE(harness.start());
    REQUIRE(blocking->started.wait_for(kThreadTimeout));
    const auto cancelled_at = Clock::now();
    harness.runner->cancel();
    REQUIRE(harness.wait_done());

    CHECK(Clock::now() - cancelled_at < std::chrono::seconds{2});
    REQUIRE(harness.outcome.result->is_error());
    CHECK(harness.outcome.result->error().kind == ErrorKind::Cancelled);
    CHECK(blocking->requests().size() == 1);
}

TEST_CASE("RequestRunner: destruirlo con el transporte bloqueado regresa en menos de 2 s",
          "[runner][hilos]") {
    auto transport = std::make_unique<BlockingTransport>(BlockingTransport::Mode::Block);
    BlockingTransport* blocking = transport.get();
    RunnerHarness harness(std::move(transport), std::make_unique<chatbot_test::FakeSleeper>());

    REQUIRE(harness.start());
    REQUIRE(blocking->started.wait_for(kThreadTimeout));
    const auto start = Clock::now();
    std::future<void> destroyed =
        std::async(std::launch::async, [&harness] { harness.runner.reset(); });
    REQUIRE(destroyed.wait_for(kThreadTimeout) == std::future_status::ready);
    CHECK(Clock::now() - start < std::chrono::seconds{2});
}

TEST_CASE("RequestRunner: reintento por contexto tras 400 con menos mensajes",
          "[runner][hilos][contexto]") {
    auto transport = std::make_unique<chatbot_test::FakeTransport>();
    chatbot_test::FakeTransport* fake = transport.get();
    fake->responses.push_back(HttpResponse{400, "{}", std::nullopt, false, ""});
    fake->responses.push_back(HttpResponse{200, sse_flow({"ok"}), std::nullopt, false, ""});
    // Límite 1000: todo cabe (622 B). La mitad (500) obliga a quitar la vuelta vieja.
    RunnerHarness harness(std::move(transport), std::make_unique<chatbot_test::FakeSleeper>(),
                          1000);

    REQUIRE(harness.start(history_with_old_turn()));
    REQUIRE(harness.wait_done());

    REQUIRE(harness.outcome.result->is_ok());
    REQUIRE(fake->requests.size() == 2);
    CHECK(sent_messages(fake->requests[0]) == 4);
    CHECK(sent_messages(fake->requests[1]) == 2);
    CHECK(harness.outcome.dropped == 2);
}

TEST_CASE("RequestRunner: con límite 0 el reintento usa la mitad del tamaño actual",
          "[runner][hilos][contexto]") {
    auto transport = std::make_unique<chatbot_test::FakeTransport>();
    chatbot_test::FakeTransport* fake = transport.get();
    fake->responses.push_back(HttpResponse{400, "{}", std::nullopt, false, ""});
    fake->responses.push_back(HttpResponse{200, sse_flow({"ok"}), std::nullopt, false, ""});
    RunnerHarness harness(std::move(transport), std::make_unique<chatbot_test::FakeSleeper>(), 0);

    REQUIRE(harness.start(history_with_old_turn()));
    REQUIRE(harness.wait_done());

    REQUIRE(harness.outcome.result->is_ok());
    REQUIRE(fake->requests.size() == 2);
    CHECK(sent_messages(fake->requests[1]) < sent_messages(fake->requests[0]));
    CHECK(harness.outcome.dropped > 0);
}

TEST_CASE("RequestRunner: sin nada más que quitar, el 400 se devuelve sin reintento",
          "[runner][hilos][contexto]") {
    auto transport = std::make_unique<chatbot_test::FakeTransport>();
    chatbot_test::FakeTransport* fake = transport.get();
    fake->responses.push_back(HttpResponse{400, "{}", std::nullopt, false, ""});
    RunnerHarness harness(std::move(transport), std::make_unique<chatbot_test::FakeSleeper>(),
                          1000);

    REQUIRE(harness.start()); // Sistema + usuario: no hay vueltas viejas.
    REQUIRE(harness.wait_done());

    REQUIRE(harness.outcome.result->is_error());
    CHECK(harness.outcome.result->error().kind == ErrorKind::InvalidRequest);
    CHECK(fake->requests.size() == 1);
}

TEST_CASE("RequestRunner: si ya se entregó texto no hay reintento por contexto",
          "[runner][hilos][contexto]") {
    auto transport = std::make_unique<BlockingTransport>(
        BlockingTransport::Mode::Respond, HttpResponse{400, "{}", std::nullopt, false, ""});
    transport->first_chunk = "data: {\"choices\":[{\"delta\":{\"content\":\"parcial\"}}]}\n\n";
    BlockingTransport* blocking = transport.get();
    RunnerHarness harness(std::move(transport), std::make_unique<chatbot_test::FakeSleeper>(),
                          1000);

    REQUIRE(harness.start(history_with_old_turn()));
    REQUIRE(harness.wait_done());

    REQUIRE(harness.outcome.result->is_error());
    CHECK(harness.outcome.result->error().kind == ErrorKind::InvalidRequest);
    CHECK(harness.outcome.deltas == std::vector<std::string>{"parcial"});
    CHECK(blocking->requests().size() == 1);
}

TEST_CASE("RequestRunner: nunca más de un reintento por contexto",
          "[runner][hilos][contexto]") {
    auto transport = std::make_unique<chatbot_test::FakeTransport>();
    chatbot_test::FakeTransport* fake = transport.get();
    fake->responses.push_back(HttpResponse{400, "{}", std::nullopt, false, ""});
    RunnerHarness harness(std::move(transport), std::make_unique<chatbot_test::FakeSleeper>(),
                          1000);

    REQUIRE(harness.start(history_with_old_turn()));
    REQUIRE(harness.wait_done());

    REQUIRE(harness.outcome.result->is_error());
    CHECK(harness.outcome.result->error().kind == ErrorKind::InvalidRequest);
    CHECK(fake->requests.size() == 2);
    CHECK(harness.outcome.done_calls == 1);
}

TEST_CASE("RequestRunner: se puede lanzar otra petición al terminar la anterior",
          "[runner][hilos]") {
    auto transport = std::make_unique<chatbot_test::FakeTransport>();
    chatbot_test::FakeTransport* fake = transport.get();
    fake->responses.push_back(HttpResponse{200, sse_flow({"uno"}), std::nullopt, false, ""});
    fake->responses.push_back(HttpResponse{200, sse_flow({"dos"}), std::nullopt, false, ""});
    RunnerHarness harness(std::move(transport), std::make_unique<chatbot_test::FakeSleeper>());

    REQUIRE(harness.start());
    REQUIRE(harness.wait_done());
    REQUIRE(harness.start());
    REQUIRE(harness.queue.run_until([&] { return harness.outcome.done_calls == 2; }));
    CHECK(harness.outcome.deltas == std::vector<std::string>{"uno", "dos"});
    CHECK(fake->requests.size() == 2);
}

TEST_CASE("RequestRunner: on_done recibe el finish_reason", "[runner][hilos]") {
    auto transport = std::make_unique<chatbot_test::FakeTransport>();
    transport->responses.push_back(HttpResponse{
        200,
        "data: {\"choices\":[{\"delta\":{\"content\":\"Hola\"},\"finish_reason\":\"length\"}]}"
        "\n\ndata: [DONE]\n\n",
        std::nullopt, false, ""});
    RunnerHarness harness(std::move(transport), std::make_unique<chatbot_test::FakeSleeper>());

    REQUIRE(harness.start());
    REQUIRE(harness.wait_done());
    REQUIRE(harness.outcome.result->is_ok());
    CHECK(harness.outcome.result->value().finish_reason == "length");
}

TEST_CASE("RequestRunner: reintento por contexto con historial menor que el límite",
          "[runner][hilos][contexto]") {
    auto transport = std::make_unique<chatbot_test::FakeTransport>();
    chatbot_test::FakeTransport* fake = transport.get();
    fake->responses.push_back(HttpResponse{400, "{}", std::nullopt, false, ""});
    fake->responses.push_back(HttpResponse{200, sse_flow({"ok"}), std::nullopt, false, ""});
    // Historial de ~5 KB con límite de 32 KB: la mitad del límite (16 KB) no
    // quitaría nada; la mitad del tamaño real sí.
    const std::vector<Message> history{
        Message{Role::System, "sistema"},
        Message{Role::User, std::string(1500, 'u')},
        Message{Role::Assistant, std::string(1500, 'a')},
        Message{Role::User, std::string(1000, 'u')},
        Message{Role::Assistant, std::string(1000, 'a')},
        Message{Role::User, "pregunta actual"},
    };
    RunnerHarness harness(std::move(transport), std::make_unique<chatbot_test::FakeSleeper>(),
                          32000);

    REQUIRE(harness.start(history));
    REQUIRE(harness.wait_done());

    REQUIRE(harness.outcome.result->is_ok());
    REQUIRE(fake->requests.size() == 2);
    CHECK(sent_messages(fake->requests[0]) == 6);
    CHECK(sent_messages(fake->requests[1]) < 6);
    CHECK(harness.outcome.dropped > 0);
}
