// ModelsLoader: GET /models en un hilo de trabajo, resultado vía post,
// cancelación y peticiones que se reemplazan. Debe pasar con tsan.

#include "models_loader.h"

#include "blocking_transport.hpp"
#include "fake_transport.hpp"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

using chatbot::HttpResponse;
using chatbot::cli::ModelsLoader;
using chatbot_test::BlockingTransport;
using chatbot_test::TaskQueue;

/// Reenvía al transporte compartido de la prueba (el loader crea uno por
/// petición y se queda con él).
class Forward final : public chatbot::Transport {
public:
    explicit Forward(std::shared_ptr<BlockingTransport> target) : target_(std::move(target)) {}
    [[nodiscard]] HttpResponse send(const chatbot::HttpRequest& request) override {
        return target_->send(request);
    }
    [[nodiscard]] HttpResponse send_stream(const chatbot::HttpRequest& request,
                                           const chatbot::StreamCallback& on_chunk) override {
        return target_->send_stream(request, on_chunk);
    }

private:
    std::shared_ptr<BlockingTransport> target_;
};

HttpResponse ok_models() {
    HttpResponse response;
    response.status = 200;
    response.body = R"({"data":[{"id":"b"},{"id":"a"}]})";
    return response;
}

struct Harness {
    TaskQueue queue;
    std::shared_ptr<BlockingTransport> transport;
    std::unique_ptr<ModelsLoader> loader;
    std::thread::id ui_thread = std::this_thread::get_id();

    explicit Harness(std::shared_ptr<BlockingTransport> t) : transport(std::move(t)) {
        loader = std::make_unique<ModelsLoader>(
            [this] { return std::make_unique<Forward>(transport); },
            [this](ModelsLoader::Task task) { queue.post(std::move(task)); });
    }
};

} // namespace

TEST_CASE("ModelsLoader: la lista llega por post al hilo de la interfaz", "[modelos][hilos]") {
    Harness h(std::make_shared<BlockingTransport>(BlockingTransport::Mode::Respond, ok_models()));
    std::optional<chatbot::Result<std::vector<std::string>>> result;
    bool on_ui_thread = false;
    chatbot::Config config = chatbot_test::test_config();
    config.model.clear(); // list_models no necesita modelo.
    h.loader->start(config, [&](chatbot::Result<std::vector<std::string>> r) {
        on_ui_thread = std::this_thread::get_id() == h.ui_thread;
        result = std::move(r);
    });
    CHECK(h.loader->busy());
    REQUIRE(h.queue.run_until([&] { return result.has_value(); }));
    CHECK(on_ui_thread);
    CHECK_FALSE(h.loader->busy());
    REQUIRE(result->is_ok());
    CHECK(result->value() == std::vector<std::string>{"a", "b"});
    const auto requests = h.transport->requests();
    REQUIRE(requests.size() == 1);
    CHECK(requests[0].method == chatbot::HttpMethod::Get);
    CHECK(requests[0].url == "https://pruebas.invalid/v1/models");
}

TEST_CASE("ModelsLoader: cancelar a la mitad descarta el resultado", "[modelos][hilos]") {
    Harness h(std::make_shared<BlockingTransport>(BlockingTransport::Mode::Block));
    int calls = 0;
    h.loader->start(chatbot_test::test_config(),
                    [&](const chatbot::Result<std::vector<std::string>>&) { ++calls; });
    REQUIRE(h.transport->started.wait_for(chatbot_test::kThreadTimeout));
    h.loader->cancel();
    CHECK_FALSE(h.loader->busy());
    // El destructor espera al hilo (que vio la cancelación) sin colgarse.
    h.loader.reset();
    (void)h.queue.run_until([] { return false; }, std::chrono::milliseconds{200});
    CHECK(calls == 0);
}

TEST_CASE("ModelsLoader: una petición nueva reemplaza a la que sigue en curso",
          "[modelos][hilos]") {
    auto blocking = std::make_shared<BlockingTransport>(BlockingTransport::Mode::Block);
    Harness h(blocking);
    int old_calls = 0;
    h.loader->start(chatbot_test::test_config(),
                    [&](const chatbot::Result<std::vector<std::string>>&) { ++old_calls; });
    REQUIRE(blocking->started.wait_for(chatbot_test::kThreadTimeout));
    // La segunda va a un transporte que responde.
    h.transport = std::make_shared<BlockingTransport>(BlockingTransport::Mode::Respond, ok_models());
    std::optional<chatbot::Result<std::vector<std::string>>> result;
    h.loader->start(chatbot_test::test_config(),
                    [&](chatbot::Result<std::vector<std::string>> r) { result = std::move(r); });
    REQUIRE(h.queue.run_until([&] { return result.has_value(); }));
    REQUIRE(result->is_ok());
    CHECK(old_calls == 0);
}
