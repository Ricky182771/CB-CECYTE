#include "bot_harness.hpp"
#include "performance.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using chatbot_bot_test::BotHarness;
using namespace std::chrono_literals;

constexpr int kChats = 50;

/// Configuración con los 50 usuarios autorizados.
chatbot::bot::BotConfig crowd_config() {
    auto config = chatbot_bot_test::test_bot_config();
    config.allowed_users.clear();
    for (int i = 1; i <= kChats; ++i) {
        config.allowed_users.push_back(i);
    }
    return config;
}

chatbot::bot::ClientFactory slow_clients(std::size_t pieces, std::chrono::milliseconds pause) {
    return [pieces, pause] {
        return std::make_unique<chatbot::ChatClient>(
            chatbot_test::test_config(),
            std::make_unique<chatbot_bot_test::SlowTransport>(pieces, pause),
            std::make_unique<chatbot_test::FakeSleeper>());
    };
}

/// La respuesta completa que da SlowTransport a "pregunta i".
std::string full_answer(int i, std::size_t pieces) {
    return "eco: pregunta " + std::to_string(i) + std::string(pieces - 1, '.');
}

} // namespace

TEST_CASE("concurrencia: 50 chats a la vez, cada uno con su respuesta",
          "[bot][hilos][concurrencia]") {
    BotHarness h;
    h.start(crowd_config(), slow_clients(5, 1ms));
    for (int i = 1; i <= kChats; ++i) {
        h.bot->handle(chatbot_bot_test::message(i, i, "pregunta " + std::to_string(i)));
    }
    REQUIRE(h.bot->wait_idle(2 * chatbot_test::kThreadTimeout));
    for (int i = 1; i <= kChats; ++i) {
        INFO("chat " << i);
        CHECK(h.outbox.texts_for(i) == std::vector<std::string>{full_answer(i, 5)});
    }
}

TEST_CASE("concurrencia: apagar a la mitad termina todo en menos de 2 s",
          "[bot][hilos][concurrencia]") {
    constexpr std::size_t kPieces = 20;
    BotHarness h;
    h.start(crowd_config(), slow_clients(kPieces, 3ms));
    for (int i = 1; i <= kChats; ++i) {
        h.bot->handle(chatbot_bot_test::message(i, i, "pregunta " + std::to_string(i)));
    }
    // Unas respuestas ya salieron; otras van a la mitad y la mayoría espera.
    REQUIRE(h.outbox.wait_for_count(6));

    const auto start = std::chrono::steady_clock::now();
    h.bot->shutdown();
    chatbot_test::report_latency("apagado con 50 chats", std::chrono::steady_clock::now() - start);

    const std::size_t delivered = h.outbox.sent().size();
    CHECK(delivered >= 6);
    CHECK(delivered < static_cast<std::size_t>(kChats));
    for (int i = 1; i <= kChats; ++i) {
        // Cada chat recibió su respuesta completa o nada: ni pedazos, ni la
        // respuesta de otro, ni un error por la cancelación.
        const std::vector<std::string> texts = h.outbox.texts_for(i);
        INFO("chat " << i);
        CHECK(texts.size() <= 1);
        if (!texts.empty()) {
            CHECK(texts[0] == full_answer(i, kPieces));
        }
    }
    CHECK(h.log.contains("apagado en_curso="));
    // Nada más sale después del apagado.
    h.bot->handle(chatbot_bot_test::message(1, 1, "otra"));
    CHECK(h.outbox.sent().size() == delivered);
}

TEST_CASE("concurrencia: apagar con respuestas bloqueadas a la mitad", "[bot][hilos]") {
    BotHarness h;
    auto config = crowd_config();
    config.max_concurrent_responses = 4;
    const auto blocked = std::make_shared<std::atomic<int>>(0);
    h.start(config, [blocked] {
        auto transport =
            std::make_unique<chatbot_test::BlockingTransport>(chatbot_test::BlockingTransport::Mode::Block);
        transport->first_chunk = "data: {\"choices\":[{\"delta\":{\"content\":\"parcial\"}}]}\n\n";
        ++*blocked;
        return std::make_unique<chatbot::ChatClient>(chatbot_test::test_config(),
                                                     std::move(transport),
                                                     std::make_unique<chatbot_test::FakeSleeper>());
    });
    for (int i = 1; i <= kChats; ++i) {
        h.bot->handle(chatbot_bot_test::message(i, i, "pregunta"));
    }
    // Espera a que los cuatro hilos estén dentro de una petición.
    const auto deadline = std::chrono::steady_clock::now() + chatbot_test::kThreadTimeout;
    while (blocked->load() < 4 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(5ms);
    }
    REQUIRE(blocked->load() == 4);
    std::this_thread::sleep_for(50ms);

    const auto start = std::chrono::steady_clock::now();
    h.bot.reset(); // El destructor apaga.
    chatbot_test::report_latency("destructor con 4 respuestas bloqueadas",
                                 std::chrono::steady_clock::now() - start);
    CHECK(h.outbox.sent().empty()); // La parte que llegó nunca se manda.
    CHECK(h.log.contains("apagado en_curso=4 descartadas=46"));
}

TEST_CASE("concurrencia: apagar mientras la plataforma manda una respuesta larga",
          "[bot][hilos]") {
    // Cada envío tarda 200 ms y la respuesta sale en muchos pedazos: sin
    // revisar la cancelación entre pedazos, el apagado esperaría segundos.
    BotHarness h{40};
    h.outbox.send_delay = 200ms;
    std::string long_answer;
    for (int i = 0; i < 30; ++i) {
        long_answer += "Párrafo número " + std::to_string(i) + ".\n\n";
    }
    h.start(chatbot_bot_test::test_bot_config(),
            chatbot_bot_test::scripted_clients(
                h.recorder, [long_answer](const std::vector<chatbot::Message>&) {
                    return chatbot_bot_test::ok_stream({long_answer});
                }));
    h.bot->handle(chatbot_bot_test::message(10, 1, "cuéntame"));
    REQUIRE(h.outbox.wait_for_count(1));

    const auto start = std::chrono::steady_clock::now();
    h.bot->shutdown();
    chatbot_test::report_latency("apagado durante el envío",
                                 std::chrono::steady_clock::now() - start);
    CHECK(h.outbox.sent().size() < 10);
    CHECK(h.log.contains("no_enviado_por_apagado chat=10"));
}

TEST_CASE("concurrencia: dos apagados a la vez esperan los dos", "[bot][hilos]") {
    BotHarness h;
    const auto gate = std::make_shared<chatbot_bot_test::Gate>();
    h.start(chatbot_bot_test::test_bot_config(), chatbot_bot_test::gated_clients(gate, h.recorder));
    h.bot->handle(chatbot_bot_test::message(10, 1, "uno"));
    REQUIRE(gate->wait_inside(1));
    std::thread other([&h] { h.bot->shutdown(); });
    h.bot->shutdown();
    // Al volver, ya no hay ninguna petición adentro: el hilo de trabajo terminó.
    CHECK(gate->inside() == 0);
    other.join();
    CHECK(gate->inside() == 0);
}
