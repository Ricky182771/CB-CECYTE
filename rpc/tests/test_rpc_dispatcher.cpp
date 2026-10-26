// Pruebas del Dispatcher de chatbot-rpc: secuencias exactas de mensajes, sin
// hilos propios ni stdio. La Session es la de test_session.cpp
// (session_fakes.hpp); el post mete cada tarea en una TaskQueue que la
// prueba vacía, y después de cada tarea se llama after_task, como main.

#include "dispatcher.h"
#include "protocol.h"

#include "conversation.h"
#include "search_settings.h"
#include "session.h"

#include "blocking_transport.hpp"
#include "fake_transport.hpp"
#include "session_fakes.hpp"

#include "chatbot/search_context.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using chatbot::rpc::Dispatcher;
using chatbot::rpc::Json;
using chatbot_test::BlockingTransport;
using chatbot_test::kThreadTimeout;

/// Flujo SSE con un evento por pedazo (texto ya escapado para JSON).
chatbot::HttpResponse stream_of(const std::vector<std::string>& pieces) {
    chatbot::HttpResponse response;
    response.status = 200;
    for (const std::string& piece : pieces) {
        response.body += "data: {\"choices\":[{\"delta\":{\"content\":\"" + piece + "\"}}]}\n\n";
    }
    response.body +=
        "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\ndata: [DONE]\n\n";
    return response;
}

chatbot::rpc::ServerInfo test_info() {
    chatbot::rpc::ServerInfo info;
    info.version = "0.1.0";
    info.platform = "linux";
    info.config_path = "/tmp/config.json";
    info.conversations_path = "/tmp/conversations";
    info.env_locks = {"CHAT_MODEL"};
    return info;
}

struct Rpc : chatbot_test::SessionFakes {
    chatbot_test::TaskQueue queue;
    std::vector<Json> out;
    Dispatcher::TimePoint now{};
    chatbot::rpc::ServerInfo info = test_info();
    chatbot::Config config = chatbot_test::test_config();
    std::unique_ptr<Dispatcher> dispatcher; ///< Al último: se destruye primero (join).

    void start() {
        dispatcher = std::make_unique<Dispatcher>(
            info,
            [this](chatbot::cli::Session::Post post, chatbot::cli::Session::OnFinished done) {
                return std::make_unique<chatbot::cli::Session>(environment(), config,
                                                               std::move(post), std::move(done));
            },
            [this](chatbot::cli::RequestRunner::Task task) {
                queue.post([this, task = std::move(task)] {
                    task();
                    dispatcher->after_task();
                });
            },
            [this](const Json& message) { out.push_back(message); }, [this] { return now; });
    }

    void line(const std::string& text) { dispatcher->on_line(text); }
    void call(const Json& id, const std::string& method, const Json& params = Json::object()) {
        line(Json{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}}.dump());
    }
    void notify(const std::string& method, const Json& params = Json::object()) {
        line(Json{{"jsonrpc", "2.0"}, {"method", method}, {"params", params}}.dump());
    }
    [[nodiscard]] std::vector<Json> take() { return std::exchange(out, {}); }

    /// start + initialize, y descarta su respuesta.
    void initialize() {
        start();
        call(1, "initialize", {{"protocol_version", 1}});
        REQUIRE(out.size() == 1);
        REQUIRE(out[0].contains("result"));
        out.clear();
    }

    /// Ejecuta tareas hasta que salga un mensaje con este método.
    bool run_until_method(const std::string& method) {
        const auto seen = [&] {
            for (const Json& message : out) {
                if (message.value("method", "") == method) {
                    return true;
                }
            }
            return false;
        };
        return queue.run_until(seen);
    }
    bool finish() { return run_until_method("chat.finished"); }
};

/// Un renglón por mensaje, para comparar secuencias.
std::string summary(const Json& message) {
    if (message.contains("id")) {
        return "#" + message["id"].dump() +
               (message.contains("error") ? " error " + message["error"]["code"].dump()
                                          : " result");
    }
    const std::string method = message["method"];
    const Json& params = message["params"];
    std::string text = method;
    if (method == "conversation.entry_added" || method == "conversation.entry_updated") {
        text += " " + params["index"].dump() + " " + params["entry"]["kind"].get<std::string>();
    } else if (method == "conversation.entry_delta") {
        text += " " + params["index"].dump() + " +" + params["text"].get<std::string>();
    } else if (method == "conversation.phase") {
        text += " " + params["phase"].get<std::string>();
    } else if (method == "conversation.closed") {
        text += " " + params["conversation"].get<std::string>() + " " +
                params["reason"].get<std::string>();
    } else if (method == "chat.finished") {
        text += " " + params["outcome"].get<std::string>();
    }
    return text;
}

std::vector<std::string> summaries(const std::vector<Json>& messages) {
    std::vector<std::string> lines;
    for (const Json& message : messages) {
        lines.push_back(summary(message));
    }
    return lines;
}

/// Los rev de los eventos de handle, en orden: from, from + 1, ...
void check_revs(const std::vector<Json>& messages, const std::string& handle,
                std::uint64_t from = 1) {
    for (const Json& message : messages) {
        const Json params = message.value("params", Json::object());
        if (params.value("conversation", "") == handle && params.contains("rev")) {
            CHECK(params["rev"] == from);
            ++from;
        }
    }
}

} // namespace

TEST_CASE("RPC: el ejemplo completo de PROTOCOL.md (sección 8)", "[rpc]") {
    Rpc rpc;
    rpc.responses = {stream_of({"¡Ho", "la! ¿En qué te ayudo?"})};
    rpc.start();
    rpc.line(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocol_version":1,)"
             R"("client":{"name":"chatbot-gui","version":"0.1.0"}}})");
    rpc.line(R"({"jsonrpc":"2.0","id":2,"method":"conversation.get","params":{"conversation":"c1"}})");
    rpc.line(R"({"jsonrpc":"2.0","id":3,"method":"chat.send","params":{"conversation":"c1","text":"hola"}})");
    REQUIRE(rpc.finish());
    std::vector<Json> got = rpc.take();
    REQUIRE(got.size() == 13);
    // El id y la fecha del archivo cambian en cada corrida.
    REQUIRE(got[9]["params"]["id"].is_string());
    got[9]["params"]["id"] = "<id>";

    const std::vector<Json> expected = {
        Json::parse(R"({"jsonrpc":"2.0","id":1,"result":{"protocol_version":1,
            "server":{"name":"chatbot-rpc","version":"0.1.0"},"platform":"linux","portable":false,
            "conversation":"c1","paths":{"config":"/tmp/config.json",
            "conversations":"/tmp/conversations","downloads":null},"ready":true,"problem":null,
            "warnings":[],"capabilities":{"search":true,"addons":[],"service":null}}})"),
        Json::parse(R"({"jsonrpc":"2.0","id":2,"result":{"conversation":"c1","rev":0,"id":null,
            "title":"","created_at":null,"last_model":"","phase":"idle","entries":[]}})"),
        Json::parse(R"({"jsonrpc":"2.0","id":3,"result":{"accepted":true}})"),
        Json::parse(R"({"jsonrpc":"2.0","method":"conversation.entry_added","params":{
            "conversation":"c1","rev":1,"index":0,"entry":{"kind":"user","text":"hola",
            "in_progress":false,"incomplete":false,"cancelled":false,"note":""}}})"),
        Json::parse(R"({"jsonrpc":"2.0","method":"conversation.phase","params":{
            "conversation":"c1","rev":2,"phase":"thinking"}})"),
        Json::parse(R"({"jsonrpc":"2.0","method":"conversation.entry_added","params":{
            "conversation":"c1","rev":3,"index":1,"entry":{"kind":"assistant","text":"¡Ho",
            "in_progress":true,"incomplete":false,"cancelled":false,"note":""}}})"),
        Json::parse(R"({"jsonrpc":"2.0","method":"conversation.phase","params":{
            "conversation":"c1","rev":4,"phase":"streaming"}})"),
        Json::parse(R"({"jsonrpc":"2.0","method":"conversation.entry_delta","params":{
            "conversation":"c1","rev":5,"index":1,"text":"la! ¿En qué te ayudo?"}})"),
        Json::parse(R"({"jsonrpc":"2.0","method":"conversation.entry_updated","params":{
            "conversation":"c1","rev":6,"index":1,"entry":{"kind":"assistant",
            "text":"¡Hola! ¿En qué te ayudo?","in_progress":false,"incomplete":false,
            "cancelled":false,"note":""}}})"),
        Json::parse(R"({"jsonrpc":"2.0","method":"conversation.saved","params":{
            "conversation":"c1","rev":7,"id":"<id>","title":"hola"}})"),
        Json::parse(R"({"jsonrpc":"2.0","method":"conversations.changed","params":{}})"),
        Json::parse(R"({"jsonrpc":"2.0","method":"conversation.phase","params":{
            "conversation":"c1","rev":8,"phase":"idle"}})"),
        Json::parse(R"({"jsonrpc":"2.0","method":"chat.finished","params":{
            "conversation":"c1","rev":9,"outcome":"ok","finish_reason":"stop",
            "dropped_messages":0,"error":null,"restore_text":null}})"),
    };
    for (std::size_t i = 0; i < expected.size(); ++i) {
        INFO("mensaje " << i);
        CHECK(got[i].dump() == expected[i].dump());
    }
}

TEST_CASE("RPC: chat.send con search", "[rpc]") {
    Rpc rpc;
    rpc.env.values["CHAT_SEARCH_API_KEY"] = chatbot_test::kSearchKey;
    SECTION("con resultados: fuentes después de la respuesta, y se guarda") {
        rpc.initialize();
        rpc.call(2, "chat.send",
                 {{"conversation", "c1"}, {"text", "/buscar clima"}, {"search", "clima"}});
        REQUIRE(rpc.finish());
        const std::vector<Json> got = rpc.take();
        CHECK(summaries(got) ==
              std::vector<std::string>{
                  "#2 result", "conversation.entry_added 0 user", "conversation.phase searching",
                  "conversation.phase thinking", "conversation.entry_added 1 assistant",
                  "conversation.phase streaming", "conversation.entry_updated 1 assistant",
                  "conversation.entry_added 2 sources", "conversation.saved",
                  "conversations.changed", "conversation.phase idle", "chat.finished ok"});
        check_revs(got, "c1");
        CHECK(got[1]["params"]["entry"]["text"] == "/buscar clima");
        CHECK(got[7]["params"]["entry"]["sources"].dump() ==
              R"([{"n":1,"title":"T","url":"https://ejemplo.mx","published_date":null}])");
    }
    SECTION("sin resultados: aviso, sin llamar al modelo") {
        rpc.search_responses = {chatbot_test::tavily_response(nlohmann::json::array())};
        rpc.initialize();
        rpc.call(2, "chat.send",
                 {{"conversation", "c1"}, {"text", "/buscar nada"}, {"search", "nada"}});
        REQUIRE(rpc.finish());
        const std::vector<Json> got = rpc.take();
        CHECK(summaries(got) == std::vector<std::string>{
                                    "#2 result", "conversation.entry_added 0 user",
                                    "conversation.phase searching",
                                    "conversation.entry_added 1 notice",
                                    "conversation.phase idle", "chat.finished no_results"});
        check_revs(got, "c1");
        CHECK(got[3]["params"]["entry"]["text"] == chatbot::kNoSearchResults);
        const Json& finished = got.back()["params"];
        CHECK(finished["restore_text"] == "/buscar nada");
        CHECK(finished["error"].is_null());
        REQUIRE(rpc.chat != nullptr);
        CHECK(rpc.chat->requests.empty());
    }
    SECTION("error de la búsqueda") {
        chatbot::HttpResponse unauthorized;
        unauthorized.status = 401;
        unauthorized.body = R"({"detail":{"error":"Unauthorized"}})";
        rpc.search_responses = {unauthorized};
        rpc.initialize();
        rpc.call(2, "chat.send",
                 {{"conversation", "c1"}, {"text", "/buscar clima"}, {"search", "clima"}});
        REQUIRE(rpc.finish());
        const std::vector<Json> got = rpc.take();
        CHECK(summaries(got) ==
              std::vector<std::string>{"#2 result", "conversation.entry_added 0 user",
                                       "conversation.phase searching",
                                       "conversation.entry_added 1 error",
                                       "conversation.phase idle", "chat.finished error"});
        const Json& finished = got.back()["params"];
        CHECK(finished["error"]["kind"] == "auth");
        CHECK(finished["error"]["http_status"] == 401);
        CHECK(finished["restore_text"] == "/buscar clima");
    }
}

TEST_CASE("RPC: chat.send rechazado al instante", "[rpc]") {
    Rpc rpc;
    SECTION("sin configuración") {
        rpc.config.model.clear();
        rpc.initialize();
        rpc.call(2, "chat.send", {{"conversation", "c1"}, {"text", "hola"}});
        const std::vector<Json> got = rpc.take();
        REQUIRE(got.size() == 1);
        CHECK(got[0]["error"]["code"] == -32004);
        CHECK(got[0]["error"]["data"]["message"] == chatbot::cli::kSetupNotice);
    }
    SECTION("texto vacío y consulta vacía") {
        rpc.initialize();
        rpc.call(2, "chat.send", {{"conversation", "c1"}, {"text", "  "}});
        rpc.call(3, "chat.send", {{"conversation", "c1"}, {"text", "x"}, {"search", ""}});
        const std::vector<Json> got = rpc.take();
        REQUIRE(got.size() == 2);
        for (const Json& error : got) {
            CHECK(error["error"]["code"] == -32005);
            CHECK(error["error"]["data"]["message"] == "El mensaje está vacío.");
        }
    }
    SECTION("sin key de búsqueda") {
        rpc.initialize();
        rpc.call(2, "chat.send", {{"conversation", "c1"}, {"text", "x"}, {"search", "x"}});
        const std::vector<Json> got = rpc.take();
        REQUIRE(got.size() == 1);
        CHECK(got[0]["error"]["code"] == -32005);
        CHECK(got[0]["error"]["data"]["message"] == chatbot::cli::kMissingSearchKey);
    }
    SECTION("un comando con / va al modelo tal cual") {
        rpc.initialize();
        rpc.call(2, "chat.send", {{"conversation", "c1"}, {"text", "/copiar"}});
        REQUIRE(rpc.finish());
        const std::vector<Json> got = rpc.take();
        CHECK(got[0]["result"]["accepted"] == true);
        CHECK(got[1]["params"]["entry"]["text"] == "/copiar");
        CHECK(got.back()["params"]["outcome"] == "ok");
    }
}

TEST_CASE("RPC: cancelar en cada fase", "[rpc]") {
    Rpc rpc;
    SECTION("searching") {
        rpc.env.values["CHAT_SEARCH_API_KEY"] = chatbot_test::kSearchKey;
        BlockingTransport* blocking = nullptr;
        rpc.search_transport = [&blocking] {
            auto transport = std::make_unique<BlockingTransport>(BlockingTransport::Mode::Block);
            blocking = transport.get();
            return transport;
        };
        rpc.initialize();
        rpc.call(2, "chat.send", {{"conversation", "c1"}, {"text", "/buscar x"}, {"search", "x"}});
        REQUIRE(blocking->started.wait_for(kThreadTimeout));
        rpc.call(3, "chat.cancel", {{"conversation", "c1"}});
        REQUIRE(rpc.finish());
        CHECK(summaries(rpc.take()) ==
              std::vector<std::string>{"#2 result", "conversation.entry_added 0 user",
                                       "conversation.phase searching", "#3 result",
                                       "conversation.entry_added 1 notice",
                                       "conversation.phase idle", "chat.finished cancelled"});
    }
    SECTION("thinking") {
        BlockingTransport* blocking = nullptr;
        rpc.chat_transport = [&blocking] {
            auto transport = std::make_unique<BlockingTransport>(BlockingTransport::Mode::Block);
            blocking = transport.get();
            return transport;
        };
        rpc.initialize();
        rpc.call(2, "chat.send", {{"conversation", "c1"}, {"text", "hola"}});
        REQUIRE(blocking->started.wait_for(kThreadTimeout));
        rpc.call(3, "chat.cancel", {{"conversation", "c1"}});
        REQUIRE(rpc.finish());
        const std::vector<Json> got = rpc.take();
        CHECK(summaries(got) ==
              std::vector<std::string>{"#2 result", "conversation.entry_added 0 user",
                                       "conversation.phase thinking", "#3 result",
                                       "conversation.entry_added 1 notice",
                                       "conversation.phase idle", "chat.finished cancelled"});
        CHECK(got[3]["result"]["cancelled"] == true);
        CHECK(got.back()["params"]["restore_text"] == "hola");
        CHECK(got.back()["params"]["error"].is_null());
    }
    SECTION("streaming") {
        BlockingTransport* blocking = nullptr;
        rpc.block(blocking);
        rpc.initialize();
        rpc.call(2, "chat.send", {{"conversation", "c1"}, {"text", "hola"}});
        REQUIRE(rpc.queue.run_until([&] { return rpc.out.size() >= 5; }));
        rpc.call(3, "chat.cancel", {{"conversation", "c1"}});
        REQUIRE(rpc.finish());
        const std::vector<Json> got = rpc.take();
        CHECK(summaries(got) ==
              std::vector<std::string>{
                  "#2 result", "conversation.entry_added 0 user", "conversation.phase thinking",
                  "conversation.entry_added 1 assistant", "conversation.phase streaming",
                  "#3 result", "conversation.entry_updated 1 assistant",
                  "conversation.phase idle", "chat.finished cancelled"});
        check_revs(got, "c1");
        CHECK(got[6]["params"]["entry"]["cancelled"] == true);
        CHECK(got[6]["params"]["entry"]["text"] == "parcial");
    }
    SECTION("sin nada en curso") {
        rpc.initialize();
        rpc.call(2, "chat.cancel", {{"conversation", "c1"}});
        CHECK(rpc.take()[0]["result"]["cancelled"] == false);
    }
}

TEST_CASE("RPC: dos chat.send seguidos → Busy", "[rpc]") {
    Rpc rpc;
    BlockingTransport* blocking = nullptr;
    rpc.block(blocking);
    rpc.initialize();
    rpc.call(2, "chat.send", {{"conversation", "c1"}, {"text", "uno"}});
    rpc.call(3, "chat.send", {{"conversation", "c1"}, {"text", "dos"}});
    const std::vector<Json> got = rpc.take();
    CHECK(summaries(got) == std::vector<std::string>{"#2 result", "conversation.entry_added 0 user",
                                                     "conversation.phase thinking",
                                                     "#3 error -32002"});
    CHECK(got[3]["error"]["data"]["message"] == chatbot::cli::kWaitOrCancel);
    rpc.call(4, "chat.cancel", {{"conversation", "c1"}});
    REQUIRE(rpc.finish());
}

TEST_CASE("RPC: 429 con retry_after_seconds", "[rpc]") {
    Rpc rpc;
    chatbot::HttpResponse limited;
    limited.status = 429;
    limited.retry_after = "20";
    limited.body = R"({"error":{"message":"demasiadas peticiones"}})";
    rpc.responses = {limited};
    rpc.initialize();
    rpc.call(2, "chat.send", {{"conversation", "c1"}, {"text", "hola"}});
    REQUIRE(rpc.finish());
    const std::vector<Json> got = rpc.take();
    CHECK(summaries(got) == std::vector<std::string>{"#2 result", "conversation.entry_added 0 user",
                                                     "conversation.phase thinking",
                                                     "conversation.entry_added 1 error",
                                                     "conversation.phase idle",
                                                     "chat.finished error"});
    const Json& finished = got.back()["params"];
    CHECK(finished["error"]["kind"] == "rate_limited");
    CHECK(finished["error"]["http_status"] == 429);
    CHECK(finished["error"]["retry_after_seconds"] == 20);
    CHECK(finished["restore_text"] == "hola");
    CHECK(finished["finish_reason"] == "");
}

TEST_CASE("RPC: respuesta sin texto → error bad_response", "[rpc]") {
    Rpc rpc;
    chatbot::HttpResponse empty;
    empty.status = 200;
    empty.body = "data: [DONE]\n\n";
    rpc.responses = {empty};
    rpc.initialize();
    rpc.call(2, "chat.send", {{"conversation", "c1"}, {"text", "hola"}});
    REQUIRE(rpc.finish());
    const Json& finished = rpc.out.back()["params"];
    CHECK(finished["outcome"] == "error");
    CHECK(finished["error"]["kind"] == "bad_response");
    CHECK(finished["error"]["message"] == chatbot::cli::kEmptyAnswer);
    CHECK(finished["restore_text"] == "hola");
}

TEST_CASE("RPC: conversation.new", "[rpc]") {
    Rpc rpc;
    SECTION("sin respuesta en curso: closed antes de la respuesta, y el handle viejo ya no existe") {
        rpc.initialize();
        rpc.call(2, "chat.send", {{"conversation", "c1"}, {"text", "hola"}});
        REQUIRE(rpc.finish());
        (void)rpc.take();
        rpc.call(3, "conversation.new");
        std::vector<Json> got = rpc.take();
        CHECK(summaries(got) ==
              std::vector<std::string>{"conversation.closed c1 replaced", "#3 result"});
        CHECK(got[1]["result"].dump() ==
              R"({"conversation":"c2","rev":0,"id":null,"title":"","created_at":null,)"
              R"("last_model":"","phase":"idle","entries":[]})");
        rpc.call(4, "conversation.get", {{"conversation", "c1"}});
        rpc.call(5, "chat.send", {{"conversation", "c1"}, {"text", "x"}});
        rpc.call(6, "chat.cancel", {{"conversation", "c1"}});
        rpc.call(7, "app.status");
        got = rpc.take();
        CHECK(summaries(got) == std::vector<std::string>{"#4 error -32003", "#5 error -32003",
                                                         "#6 error -32003", "#7 result"});
        CHECK(got[3]["result"]["conversation"] == "c2");
        // La nueva empieza en rev 1.
        rpc.call(8, "chat.send", {{"conversation", "c2"}, {"text", "otra"}});
        REQUIRE(rpc.finish());
        check_revs(rpc.take(), "c2");
    }
    SECTION("con respuesta en curso: Busy") {
        BlockingTransport* blocking = nullptr;
        rpc.block(blocking);
        rpc.initialize();
        rpc.call(2, "chat.send", {{"conversation", "c1"}, {"text", "hola"}});
        (void)rpc.take();
        rpc.call(3, "conversation.new");
        const std::vector<Json> got = rpc.take();
        REQUIRE(got.size() == 1);
        CHECK(got[0]["error"]["code"] == -32002);
        CHECK(got[0]["error"]["data"]["message"] == chatbot::cli::kWaitOrCancel);
        rpc.call(4, "chat.cancel", {{"conversation", "c1"}});
        REQUIRE(rpc.finish());
    }
}

TEST_CASE("RPC: guardado que falla: sin conversation.saved, con la entrada de error", "[rpc]") {
    Rpc rpc;
    std::ofstream{rpc.path("conversations")} << "x"; // La carpeta es un archivo.
    rpc.initialize();
    rpc.call(2, "chat.send", {{"conversation", "c1"}, {"text", "hola"}});
    REQUIRE(rpc.finish());
    const std::vector<Json> got = rpc.take();
    CHECK(summaries(got) ==
          std::vector<std::string>{"#2 result", "conversation.entry_added 0 user",
                                   "conversation.phase thinking",
                                   "conversation.entry_added 1 assistant",
                                   "conversation.phase streaming",
                                   "conversation.entry_updated 1 assistant",
                                   "conversation.entry_added 2 error",
                                   "conversation.phase idle", "chat.finished ok"});
    CHECK(got[6]["params"]["entry"]["text"].get<std::string>().rfind(
              "No se pudo guardar la conversación: ", 0) == 0);
}

TEST_CASE("RPC: deltas agrupados en una ventana de 30 ms", "[rpc]") {
    Rpc rpc;
    SECTION("50 deltas en menos de 30 ms: entry_added con el primero y un solo entry_delta") {
        std::vector<std::string> pieces;
        std::string all;
        for (int i = 0; i < 50; ++i) {
            pieces.push_back(std::to_string(i) + " ");
            all += pieces.back();
        }
        rpc.responses = {stream_of(pieces)};
        rpc.initialize();
        rpc.call(2, "chat.send", {{"conversation", "c1"}, {"text", "cuenta"}});
        REQUIRE(rpc.finish()); // El reloj falso no avanza: todo cae en la ventana.
        const std::vector<Json> got = rpc.take();
        CHECK(summaries(got) ==
              std::vector<std::string>{
                  "#2 result", "conversation.entry_added 0 user", "conversation.phase thinking",
                  "conversation.entry_added 1 assistant", "conversation.phase streaming",
                  "conversation.entry_delta 1 +" + all.substr(2),
                  "conversation.entry_updated 1 assistant", "conversation.saved",
                  "conversations.changed", "conversation.phase idle", "chat.finished ok"});
        check_revs(got, "c1");
    }
    SECTION("pasada la ventana, flush_due lo vacía; un conversation.get a la mitad también") {
        BlockingTransport* blocking = nullptr;
        rpc.chat_transport = [&blocking] {
            auto transport = std::make_unique<BlockingTransport>(BlockingTransport::Mode::Block);
            transport->first_chunk =
                "data: {\"choices\":[{\"delta\":{\"content\":\"uno \"}}]}\n\n"
                "data: {\"choices\":[{\"delta\":{\"content\":\"dos \"}}]}\n\n"
                "data: {\"choices\":[{\"delta\":{\"content\":\"tres \"}}]}\n\n";
            blocking = transport.get();
            return transport;
        };
        rpc.initialize();
        rpc.call(2, "chat.send", {{"conversation", "c1"}, {"text", "hola"}});
        // Tres tareas de delta: la primera crea la entrada, las otras quedan pendientes.
        REQUIRE(rpc.queue.run_until([&] { return rpc.dispatcher->next_deadline().has_value(); }));
        REQUIRE(rpc.queue.run_until([&] {
            return rpc.out.size() == 5 && rpc.out[3]["params"]["entry"]["text"] == "uno ";
        }));
        // Antes de la ventana no sale nada.
        const Dispatcher::TimePoint deadline = *rpc.dispatcher->next_deadline();
        CHECK(deadline == rpc.now + chatbot::rpc::kDeltaWindow);
        rpc.now += std::chrono::milliseconds{29};
        rpc.dispatcher->flush_due();
        CHECK(rpc.out.size() == 5);

        // El get vacía el pendiente antes de responder: el snapshot coincide.
        REQUIRE(rpc.queue.run_until([] { return false; }, std::chrono::milliseconds{50}) == false);
        rpc.call(3, "conversation.get", {{"conversation", "c1"}});
        std::vector<Json> got = rpc.take();
        REQUIRE(got.size() == 7);
        CHECK(summary(got[5]) == "conversation.entry_delta 1 +dos tres ");
        const Json& snapshot = got[6]["result"];
        CHECK(snapshot["rev"] == got[5]["params"]["rev"]);
        CHECK(snapshot["phase"] == "streaming");
        CHECK(snapshot["entries"][1]["text"] ==
              got[3]["params"]["entry"]["text"].get<std::string>() +
                  got[5]["params"]["text"].get<std::string>());
        CHECK_FALSE(rpc.dispatcher->next_deadline().has_value());
        rpc.call(4, "chat.cancel", {{"conversation", "c1"}});
        REQUIRE(rpc.finish());
    }
}

TEST_CASE("RPC: flush_due vacía el delta al pasar la ventana", "[rpc]") {
    Rpc rpc;
    BlockingTransport* blocking = nullptr;
    rpc.chat_transport = [&blocking] {
        auto transport = std::make_unique<BlockingTransport>(BlockingTransport::Mode::Block);
        transport->first_chunk = "data: {\"choices\":[{\"delta\":{\"content\":\"a\"}}]}\n\n"
                                 "data: {\"choices\":[{\"delta\":{\"content\":\"b\"}}]}\n\n";
        blocking = transport.get();
        return transport;
    };
    rpc.initialize();
    rpc.call(2, "chat.send", {{"conversation", "c1"}, {"text", "hola"}});
    REQUIRE(rpc.queue.run_until([&] { return rpc.dispatcher->next_deadline().has_value(); }));
    rpc.now += chatbot::rpc::kDeltaWindow;
    rpc.dispatcher->flush_due();
    CHECK(summary(rpc.out.back()) == "conversation.entry_delta 1 +b");
    CHECK_FALSE(rpc.dispatcher->next_deadline().has_value());
    rpc.call(3, "chat.cancel", {{"conversation", "c1"}});
    REQUIRE(rpc.finish());
}

TEST_CASE("RPC: ciclo de vida y errores del protocolo", "[rpc]") {
    Rpc rpc;
    SECTION("StartupFailed: initialize falla; shutdown y exit funcionan") {
        rpc.info.startup_error = "[error de configuración] Archivo de configuración inválido";
        rpc.start();
        rpc.call(1, "initialize", {{"protocol_version", 1}});
        rpc.call(2, "app.status");
        rpc.call(3, "shutdown");
        rpc.notify("exit");
        const std::vector<Json> got = rpc.take();
        CHECK(summaries(got) == std::vector<std::string>{"#1 error -32011", "#2 error -32001",
                                                         "#3 result"});
        CHECK(got[0]["error"]["data"]["message"] ==
              "[error de configuración] Archivo de configuración inválido");
        CHECK(rpc.dispatcher->exit_code() == 0);
    }
    SECTION("métodos antes de initialize y versión distinta") {
        rpc.start();
        rpc.call(1, "conversation.get", {{"conversation", "c1"}});
        rpc.call(2, "initialize", {{"protocol_version", 2}});
        rpc.call(3, "initialize", {{"protocol_version", 1}});
        rpc.call(4, "initialize", {{"protocol_version", 1}});
        const std::vector<Json> got = rpc.take();
        CHECK(summaries(got) == std::vector<std::string>{"#1 error -32001", "#2 error -32010",
                                                         "#3 result", "#4 error -32600"});
        CHECK(got[1]["error"]["data"].dump() == R"({"server":1})");
    }
    SECTION("sin configuración: ready false con el aviso") {
        rpc.config.model.clear();
        rpc.start();
        rpc.call(1, "initialize", {{"protocol_version", 1}});
        rpc.call(2, "app.status");
        const std::vector<Json> got = rpc.take();
        CHECK(got[0]["result"]["ready"] == false);
        CHECK(got[0]["result"]["problem"] == chatbot::cli::kSetupNotice);
        CHECK(got[1]["result"].dump() ==
              R"({"ready":false,"problem":"Configura un proveedor para empezar.","busy":false,)"
              R"("conversation":"c1","env_locks":["CHAT_MODEL"],"has_search_key":false})");
    }
    SECTION("shutdown y luego otra petición; exit con 0") {
        rpc.initialize();
        rpc.call(2, "shutdown");
        rpc.call(3, "app.status");
        rpc.call(4, "exit");
        const std::vector<Json> got = rpc.take();
        CHECK(summaries(got) ==
              std::vector<std::string>{"conversation.closed c1 shutdown", "#2 result",
                                       "#3 error -32600", "#4 result"});
        CHECK(rpc.dispatcher->exit_code() == 0);
    }
    SECTION("exit sin shutdown: código 1") {
        rpc.initialize();
        CHECK_FALSE(rpc.dispatcher->exit_code().has_value());
        rpc.notify("exit");
        CHECK(rpc.out.empty());
        CHECK(rpc.dispatcher->exit_code() == 1);
    }
    SECTION("shutdown con una respuesta en curso: closed y result, sin chat.finished") {
        BlockingTransport* blocking = nullptr;
        rpc.block(blocking);
        rpc.initialize();
        rpc.call(2, "chat.send", {{"conversation", "c1"}, {"text", "hola"}});
        REQUIRE(blocking->started.wait_for(kThreadTimeout));
        (void)rpc.take();
        rpc.call(3, "shutdown"); // Cancela y hace join.
        // Lo que el runner dejó en la cola ya no corre.
        CHECK_FALSE(rpc.queue.run_until([] { return false; }, std::chrono::milliseconds{100}));
        CHECK(summaries(rpc.take()) ==
              std::vector<std::string>{"conversation.closed c1 shutdown", "#3 result"});
    }
    SECTION("EOF a la mitad de una respuesta: nada sale y lo encolado no corre") {
        BlockingTransport* blocking = nullptr;
        rpc.block(blocking);
        rpc.initialize();
        rpc.call(2, "chat.send", {{"conversation", "c1"}, {"text", "hola"}});
        REQUIRE(blocking->started.wait_for(kThreadTimeout));
        (void)rpc.take();
        rpc.dispatcher->on_eof();
        CHECK_FALSE(rpc.queue.run_until([] { return false; }, std::chrono::milliseconds{100}));
        CHECK(rpc.out.empty());
    }
    SECTION("parámetros de más, de otro tipo o que faltan") {
        rpc.initialize();
        rpc.call(2, "app.status", {{"extra", 1}});
        rpc.call(3, "chat.send", {{"conversation", "c1"}, {"text", 5}, {"otro", "x"}});
        rpc.call(4, "conversation.get");
        const std::vector<Json> got = rpc.take();
        CHECK(summaries(got) == std::vector<std::string>{"#2 error -32602", "#3 error -32602",
                                                         "#4 error -32602"});
        CHECK(got[0]["error"]["data"].dump() == R"({"fields":["extra"]})");
        CHECK(got[1]["error"]["data"].dump() == R"({"fields":["otro","text"]})");
        CHECK(got[2]["error"]["data"].dump() == R"({"fields":["conversation"]})");
    }
    SECTION("JSON roto, lote, id inválido, sin jsonrpc, línea de más de 1 MiB") {
        rpc.initialize();
        rpc.line("{no es json");
        rpc.line(R"([{"jsonrpc":"2.0","id":2,"method":"app.status"}])");
        rpc.line(R"({"jsonrpc":"2.0","id":1.5,"method":"app.status"})");
        rpc.line(R"({"jsonrpc":"2.0","id":null,"method":"app.status"})");
        rpc.line(R"({"id":3,"method":"app.status"})");
        rpc.line(R"({"jsonrpc":"2.0","method":5})");
        rpc.dispatcher->on_oversized_line();
        const std::vector<Json> got = rpc.take();
        CHECK(summaries(got) ==
              std::vector<std::string>{"#null error -32700", "#null error -32600",
                                       "#null error -32600", "#null error -32600",
                                       "#3 error -32600", "#null error -32600",
                                       "#null error -32600"});
    }
    SECTION("notificaciones: sin respuesta, ni de error; $/cancel se acepta") {
        rpc.initialize();
        rpc.notify("algo.desconocido");
        rpc.notify("app.status", {{"extra", 1}});
        rpc.notify("$/cancel", {{"id", 7}});
        rpc.call(2, "$/cancel", {{"id", "x"}});
        const std::vector<Json> got = rpc.take();
        CHECK(summaries(got) == std::vector<std::string>{"#2 result"});
    }
    SECTION("un método de R2 en adelante: MethodNotFound; el id puede ser cadena") {
        rpc.initialize();
        rpc.call("a", "conversations.list");
        rpc.call("b", "settings.provider.get");
        const std::vector<Json> got = rpc.take();
        CHECK(summaries(got) ==
              std::vector<std::string>{R"(#"a" error -32601)", R"(#"b" error -32601)"});
    }
}
