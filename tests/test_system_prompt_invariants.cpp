// Invariantes del mensaje de sistema de extremo a extremo, sin red:
// Conversation + RequestRunner + FakeTransport, revisados sobre el cuerpo
// JSON de cada petición (lo que ve el modelo).

#include "conversation.h"
#include "request_runner.h"
#include "search_context.h"

#include "blocking_transport.hpp"
#include "fake_search_provider.hpp"
#include "fake_transport.hpp"
#include "system_prompt_invariants.hpp"

#include "chatbot/chat_client.h"
#include "chatbot/history.h"
#include "chatbot/tavily_search.h"

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using chatbot::HttpResponse;
using chatbot::Message;
using chatbot::Role;
using chatbot::cli::Conversation;
using chatbot::cli::RequestRunner;
using chatbot_test::check_invariants;
using chatbot_test::content_bytes;
using chatbot_test::Ending;
using chatbot_test::sent_messages_of;

std::string sse_answer(const std::string& text) {
    return "data: {\"choices\":[{\"delta\":{\"content\":\"" + text +
           "\"},\"finish_reason\":\"stop\"}]}\n\ndata: [DONE]\n\n";
}

/// Proveedor de búsqueda falso: TavilySearch con un FakeTransport que
/// responde con los resultados dados.
std::shared_ptr<chatbot::SearchProvider> search_provider(const nlohmann::json& results) {
    auto transport = std::make_unique<chatbot_test::FakeTransport>();
    transport->responses.push_back(chatbot_test::tavily_response(results));
    return std::make_shared<chatbot::TavilySearch>("clave-ficticia", std::move(transport));
}

/// Lo que pasó en un turno.
struct TurnOutcome {
    std::vector<Message> history;  ///< El historial completo que recibió el runner.
    std::vector<Message> sent;     ///< Los messages del cuerpo de la última petición.
    std::size_t requests = 0;      ///< Peticiones al modelo en este turno.
    std::size_t dropped = 0;       ///< El dropped que recibió on_done.
    std::optional<std::string> restored; ///< Texto que regresa a la caja (error).
};

/// La app sin FTXUI: el mismo cableado que main.cpp entre Conversation y
/// RequestRunner, con una cola en lugar de screen.Post.
struct Session {
    chatbot_test::TaskQueue queue;
    std::unique_ptr<chatbot::ChatClient> client;
    chatbot_test::FakeTransport* model = nullptr;
    std::unique_ptr<RequestRunner> runner;
    Conversation conversation;

    Session(std::string_view prompt, std::size_t limit) : conversation{prompt} {
        auto transport = std::make_unique<chatbot_test::FakeTransport>();
        model = transport.get();
        model->responses.push_back(HttpResponse{200, sse_answer("ok"), std::nullopt, false, ""});
        client = std::make_unique<chatbot::ChatClient>(
            chatbot_test::test_config(), std::move(transport),
            std::make_unique<chatbot_test::FakeSleeper>());
        runner = std::make_unique<RequestRunner>(
            *client, limit, [this](RequestRunner::Task task) { queue.post(std::move(task)); });
    }

    /// Envía text como lo hace main.cpp. Con provider, es un /buscar de query.
    TurnOutcome turn(const std::string& text,
                     std::shared_ptr<chatbot::SearchProvider> provider = nullptr,
                     const std::string& query = {}) {
        std::optional<std::vector<Message>> messages = conversation.submit(text);
        REQUIRE(messages.has_value());
        TurnOutcome outcome;
        outcome.history = *messages;
        const std::size_t before = model->requests.size();
        bool done = false;
        const auto on_delta = [this](std::string delta) { conversation.append_delta(delta); };
        const auto on_done = [this, &outcome, &done](chatbot::Result<chatbot::CompletionInfo> result,
                                                     std::size_t dropped) {
            outcome.dropped = dropped;
            outcome.restored = result.is_ok()
                                   ? conversation.finish_success(result.value().finish_reason,
                                                                 "modelo-de-prueba")
                                   : std::optional<std::string>{
                                         conversation.finish_error(result.error())};
            done = true;
        };
        if (provider == nullptr) {
            REQUIRE(runner->start(std::move(*messages), on_delta, on_done));
        } else {
            const std::string date = "2026-10-09";
            REQUIRE(runner->start_with_search(
                std::move(*messages), query, std::move(provider),
                chatbot::cli::spanish_date(date),
                [this, &outcome, date](RequestRunner::SearchContext found) {
                    // El runner mandó al modelo el bloque en lugar del comando.
                    outcome.history.back().content = found.block;
                    (void)conversation.attach_search(std::move(found.response), date,
                                                     std::move(found.block));
                },
                on_delta, on_done));
        }
        REQUIRE(queue.run_until([&done] { return done; }));
        outcome.requests = model->requests.size() - before;
        if (outcome.requests > 0) {
            outcome.sent = sent_messages_of(model->requests.back());
        }
        return outcome;
    }
};

} // namespace

TEST_CASE("Extremo a extremo: 100 turnos con límite chico y /buscar conservan el sistema",
          "[sistema][extremo]") {
    constexpr std::size_t kLimit = 4000;
    const std::string p1 = "Empieza SIEMPRE tu respuesta con la palabra PIÑA.";
    const std::string p2 = "Instrucciones nuevas: responde en una sola línea. 🍍";
    std::string prompt = p1;
    Session session{prompt, kLimit};
    // Respuestas de ~300 B: el historial pasa el límite en pocos turnos.
    session.model->responses.front().body = sse_answer("PIÑA. " + std::string(300, 'r'));

    std::size_t turns_with_dropped = 0;
    std::size_t searches = 0;
    for (int turn = 0; turn < 100; ++turn) {
        INFO("turno " << turn);
        // A la mitad cambian las instrucciones: desde la siguiente petición
        // I1 usa el prompt nuevo.
        if (turn == 50) {
            prompt = p2;
            REQUIRE(session.conversation.set_system_prompt(prompt));
        }

        TurnOutcome outcome;
        std::string expected_last;
        if (turn % 10 == 9) {
            ++searches;
            // Uno de cada dos /buscar trae un bloque mayor que el límite.
            const std::size_t content = turn % 20 == 19 ? 1200 : 400;
            const std::string query = "consulta " + std::to_string(turn);
            nlohmann::json results = nlohmann::json::array();
            for (int i = 0; i < 4; ++i) {
                results.push_back({{"title", "Resultado " + std::to_string(i)},
                                   {"url", "https://ejemplo.com/" + std::to_string(i)},
                                   {"content", std::string(content, 'c')}});
            }
            outcome = session.turn("/buscar " + query, search_provider(results), query);
            expected_last = outcome.history.back().content;
            CHECK(expected_last.rfind("Fecha de la búsqueda: ", 0) == 0);
            CHECK(expected_last.find("Pregunta del usuario: " + query + "\n") !=
                  std::string::npos);
            if (content == 1200) {
                CHECK(content_bytes({outcome.history.back()}) > kLimit);
            }
        } else {
            expected_last = "Pregunta " + std::to_string(turn) + ": " + std::string(200, 'q');
            outcome = session.turn(expected_last);
        }

        REQUIRE(outcome.requests == 1);
        REQUIRE_FALSE(outcome.restored.has_value());
        CHECK(check_invariants(outcome.sent, prompt, Ending::Sent, expected_last) == "");
        CHECK(check_invariants(session.conversation.history(), prompt, Ending::Idle) == "");

        // El aviso de mensajes quitados (main.cpp lo muestra si dropped > 0)
        // corresponde a lo que se recortó de verdad.
        const chatbot::TrimResult expected = chatbot::trim_history(outcome.history, kLimit);
        CHECK(outcome.dropped == expected.dropped);
        CHECK(outcome.dropped == outcome.history.size() - outcome.sent.size());
        const std::size_t minimum = prompt.empty() ? 1 : 2; // Sistema + último.
        const bool over = content_bytes(outcome.history) > kLimit;
        CHECK((outcome.dropped > 0) == (over && outcome.history.size() > minimum));
        CHECK((content_bytes(outcome.sent) <= kLimit || outcome.sent.size() == minimum));
        turns_with_dropped += outcome.dropped > 0 ? 1 : 0;
    }
    CHECK(searches == 10);
    CHECK(turns_with_dropped > 80);
}

TEST_CASE("Extremo a extremo: con instrucciones vacías no se manda ningún System",
          "[sistema][extremo]") {
    constexpr std::size_t kLimit = 4000;
    Session session{"", kLimit};
    session.model->responses.front().body = sse_answer(std::string(300, 'r'));
    for (int turn = 0; turn < 30; ++turn) {
        INFO("turno " << turn);
        TurnOutcome outcome;
        std::string expected_last;
        if (turn % 10 == 9) {
            const std::string query = "consulta " + std::to_string(turn);
            outcome = session.turn(
                "/buscar " + query,
                search_provider(nlohmann::json::array({{{"title", "R"},
                                                        {"url", "https://ejemplo.com"},
                                                        {"content", std::string(500, 'c')}}})),
                query);
            expected_last = outcome.history.back().content;
        } else {
            expected_last = "Pregunta " + std::to_string(turn) + " " + std::string(200, 'q');
            outcome = session.turn(expected_last);
        }
        REQUIRE(outcome.requests == 1);
        CHECK(check_invariants(outcome.sent, "", Ending::Sent, expected_last) == "");
        CHECK(outcome.dropped == chatbot::trim_history(outcome.history, kLimit).dropped);
    }
}
