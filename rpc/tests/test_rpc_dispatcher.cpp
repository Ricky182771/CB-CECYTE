// Pruebas del Dispatcher de chatbot-rpc: secuencias exactas de mensajes, sin
// hilos propios ni stdio. La Session es la de test_session.cpp
// (session_fakes.hpp); el post mete cada tarea en una TaskQueue que la
// prueba vacía, y después de cada tarea se llama after_task, como main.

#include "dispatcher.h"
#include "protocol.h"

#include "code_blocks.h"
#include "conversation.h"
#include "conversation_store.h"
#include "downloads.h"
#include "models_loader.h"
#include "providers.h"
#include "search_settings.h"
#include "session.h"
#include "sidebar.h"

#include "blocking_transport.hpp"
#include "fake_transport.hpp"
#include "session_fakes.hpp"

#include "chatbot/config.h"
#include "chatbot/platform.h"
#include "chatbot/search_context.h"
#include "chatbot/system_prompt.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
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
    /// El transporte de cada models.list.
    std::function<std::unique_ptr<chatbot::Transport>()> models_transport = [] {
        return std::make_unique<chatbot_test::FakeTransport>();
    };
    std::unique_ptr<Dispatcher> dispatcher; ///< Al último: se destruye primero (join).

    void start() {
        dispatcher = std::make_unique<Dispatcher>(
            info,
            [this](chatbot::cli::Session::Post post, chatbot::cli::Session::OnFinished done) {
                return std::make_unique<chatbot::cli::Session>(environment(), config,
                                                               std::move(post), std::move(done));
            },
            [this] { return models_transport(); },
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
    } else if (method == "settings.changed") {
        text += " " + params["section"].get<std::string>();
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
    SECTION("un método de R4 o uno desconocido: MethodNotFound; el id puede ser cadena") {
        rpc.initialize();
        rpc.call("a", "addons.list");
        rpc.call("b", "settings.appearance.get");
        const std::vector<Json> got = rpc.take();
        CHECK(summaries(got) ==
              std::vector<std::string>{R"(#"a" error -32601)", R"(#"b" error -32601)"});
    }
}

namespace {

/// Respuesta con un bloque de código ("int x;").
chatbot::HttpResponse with_block() { return stream_of({"Mira:\\n\\n```cpp\\nint x;\\n```"}); }

/// Un chat.send que termina bien; descarta lo que sale.
void exchange(Rpc& rpc, const Json& id, const std::string& handle, const std::string& text) {
    rpc.call(id, "chat.send", {{"conversation", handle}, {"text", text}});
    REQUIRE(rpc.finish());
    (void)rpc.take();
}

/// Una sola respuesta: su result (o su error).
Json only(Rpc& rpc) {
    const std::vector<Json> got = rpc.take();
    REQUIRE(got.size() == 1);
    return got[0];
}

/// El error de JSON-RPC con data.message.
void check_error(const Json& message, int code, const std::string& text) {
    CHECK(message["error"]["code"] == code);
    CHECK(message["error"]["data"]["message"] == text);
}

} // namespace

TEST_CASE("RPC: conversations.list, conversation.open y conversation.delete", "[rpc]") {
    Rpc rpc;
    rpc.config.model = "modelo-a";
    rpc.initialize();
    exchange(rpc, 2, "c1", "uno");
    rpc.call(3, "conversation.new");
    exchange(rpc, 4, "c2", "dos");
    const std::string unreadable = "20260101-000000-abcdef";
    std::ofstream{rpc.dir.path() / "conversations" / (unreadable + ".json")} << "{no es json";

    // Otro proceso con otro modelo y la misma carpeta.
    rpc.config.model = "modelo-b";
    rpc.initialize();
    rpc.call(2, "conversations.list");
    Json list = only(rpc)["result"]["items"];
    REQUIRE(list.size() == 3);
    const chatbot::cli::ConversationStore store{rpc.path("conversations")};
    const std::vector<chatbot::cli::ConversationSummary> summaries_now = store.list();
    const chatbot::cli::CalendarDay today =
        chatbot::cli::local_day(chatbot_test::kSessionNow).value();
    for (std::size_t i = 0; i < list.size(); ++i) {
        CHECK(list[i]["id"] == summaries_now[i].id);
        CHECK(list[i]["group"] == chatbot::cli::group_for(summaries_now[i], today));
        CHECK(list[i]["open_as"].is_null()); // La abierta es nueva y no está guardada.
    }
    // Las dos se guardan en el mismo segundo: su orden no es fijo.
    const std::size_t at_one = list[0]["title"] == "uno" ? 0 : 1;
    CHECK(list[at_one]["title"] == "uno");
    CHECK(list[1 - at_one]["title"] == "dos");
    CHECK(list[0]["message_count"] == 2);
    CHECK(list[0]["last_model"] == "modelo-a");
    CHECK(list[0]["readable"] == true);
    CHECK(list[0]["error"] == "");
    CHECK(list[2]["id"] == unreadable);
    CHECK(list[2]["readable"] == false);
    CHECK(list[2]["group"] == "Sin fecha");
    const std::string one = list[at_one]["id"];
    const std::string two = list[1 - at_one]["id"];

    // Abrir otra: closed antes de la respuesta, con el aviso del modelo.
    rpc.call(3, "conversation.open", {{"id", one}});
    std::vector<Json> got = rpc.take();
    CHECK(summaries(got) ==
          std::vector<std::string>{"conversation.closed c1 replaced", "#3 result"});
    const Json opened = got[1]["result"];
    CHECK(opened["conversation"] == "c2");
    CHECK(opened["rev"] == 0);
    CHECK(opened["id"] == one);
    CHECK(opened["title"] == "uno");
    CHECK(opened["entries"].back()["kind"] == "notice");
    CHECK(opened["entries"].back()["text"] ==
          "Esta conversación usó modelo-a; se continúa con modelo-b.");
    rpc.call(4, "conversations.list");
    list = only(rpc)["result"]["items"];
    for (const Json& item : list) {
        CHECK(item["open_as"] == (item["id"] == one ? Json("c2") : Json(nullptr)));
    }

    // La misma: el mismo handle, sin recargar (el aviso sigue una sola vez).
    rpc.call(5, "conversation.open", {{"id", one}});
    got = rpc.take();
    CHECK(summaries(got) == std::vector<std::string>{"#5 result"});
    CHECK(got[0]["result"] == opened);

    // Ilegible: -32007 con el error de load, y la abierta no cambia.
    rpc.call(6, "conversation.open", {{"id", unreadable}});
    check_error(only(rpc), -32007, store.load(unreadable).error);
    rpc.call(7, "app.status");
    CHECK(only(rpc)["result"]["conversation"] == "c2");

    // Borrar otra: sin closed y replaced_by null.
    rpc.call(8, "conversation.delete", {{"id", two}});
    got = rpc.take();
    CHECK(summaries(got) == std::vector<std::string>{"#8 result", "conversations.changed"});
    CHECK(got[0]["result"].dump() == R"({"replaced_by":null})");

    // Borrar la abierta: closed, y replaced_by trae la nueva.
    rpc.call(9, "conversation.delete", {{"id", one}});
    got = rpc.take();
    CHECK(summaries(got) == std::vector<std::string>{"conversation.closed c2 deleted", "#9 result",
                                                     "conversations.changed"});
    CHECK(got[1]["result"]["replaced_by"].dump() ==
          R"({"conversation":"c3","rev":0,"id":null,"title":"","created_at":null,)"
          R"("last_model":"","phase":"idle","entries":[]})");

    // Ilegible: nunca se borra.
    rpc.call(10, "conversation.delete", {{"id", unreadable}});
    const Json error = only(rpc);
    CHECK(error["error"]["code"] == -32007);
    CHECK(std::filesystem::exists(rpc.dir.path() / "conversations" / (unreadable + ".json")));
    rpc.call(11, "conversations.list");
    CHECK(only(rpc)["result"]["items"].size() == 1);
}

TEST_CASE("RPC: abrir y borrar con una respuesta en curso → Busy", "[rpc]") {
    Rpc rpc;
    BlockingTransport* blocking = nullptr;
    rpc.block(blocking);
    rpc.initialize();
    rpc.call(2, "chat.send", {{"conversation", "c1"}, {"text", "hola"}});
    (void)rpc.take();
    rpc.call(3, "conversation.open", {{"id", "20260101-000000-abcdef"}});
    rpc.call(4, "conversation.delete", {{"id", "20260101-000000-abcdef"}});
    const std::vector<Json> got = rpc.take();
    REQUIRE(got.size() == 2);
    for (const Json& message : got) {
        check_error(message, -32002, std::string{chatbot::cli::kWaitOrCancel});
    }
    rpc.call(5, "chat.cancel", {{"conversation", "c1"}});
    REQUIRE(rpc.finish());
}

TEST_CASE("RPC: commands.list con y sin configuración", "[rpc]") {
    Rpc rpc;
    SECTION("sin configuración") {
        rpc.config.model.clear();
        rpc.initialize();
        rpc.call(2, "commands.list", {{"conversation", "c1"}});
        const Json commands = only(rpc)["result"]["commands"];
        REQUIRE(commands.size() == chatbot::cli::kCommands.size());
        CHECK(commands[0].dump() ==
              R"({"name":"/buscar","arguments":"<consulta>",)"
              R"("description":"Busca en la web y responde con fuentes citadas",)"
              R"("needs_argument":true,"available":false,)"
              R"x("note":"falta configurar el proveedor (F2)"})x");
        CHECK(commands[3]["note"] == "la conversación está vacía");
    }
    SECTION("con configuración, bloques y pares") {
        rpc.responses = {with_block()};
        rpc.initialize();
        exchange(rpc, 2, "c1", "un bloque");
        rpc.call(3, "commands.list", {{"conversation", "c1"}});
        const Json commands = only(rpc)["result"]["commands"];
        CHECK(commands[0]["note"] == "falta la key de búsqueda (F2)");
        for (std::size_t i = 1; i < commands.size(); ++i) {
            CHECK(commands[i]["available"] == true);
            CHECK(commands[i]["note"] == "");
        }
        rpc.call(4, "commands.list", {{"conversation", "c9"}});
        CHECK(only(rpc)["error"]["code"] == -32003);
    }
}

TEST_CASE("RPC: commands.run", "[rpc]") {
    Rpc rpc;
    rpc.responses = {with_block()};
    const auto run = [&rpc](const Json& id, const std::string& text) {
        rpc.call(id, "commands.run", {{"conversation", "c1"}, {"text", text}});
    };
    const auto check_status = [&rpc](const std::string& text) {
        CHECK(only(rpc)["result"] ==
              Json({{"kind", "status"}, {"message", text}, {"clear", false}}));
    };
    const std::string no_dir{chatbot::cli::no_download_dir_message(chatbot::current_os())};

    SECTION("sin configuración: el texto para el modelo da -32004; lo local responde") {
        rpc.config.model.clear();
        rpc.initialize();
        run(2, "hola");
        check_error(only(rpc), -32004, std::string{chatbot::cli::kSetupNotice});
        run(3, "/buscar clima");
        check_error(only(rpc), -32004, std::string{chatbot::cli::kSetupNotice});
        run(4, "/copiar");
        check_status("No hay bloques de código en la conversación.");
        run(5, "/exportar");
        check_status("Todavía no hay respuestas que exportar.");
    }
    SECTION("errores del texto al modelo, como chat.send") {
        rpc.initialize();
        run(2, "   ");
        check_error(only(rpc), -32005, "El mensaje está vacío.");
        run(3, "/buscar   ");
        check_error(only(rpc), -32005, std::string{chatbot::cli::kSearchUsage});
        run(4, "/buscar clima");
        check_error(only(rpc), -32005, std::string{chatbot::cli::kMissingSearchKey});
    }
    SECTION("message, status, copy, save y export") {
        rpc.initialize();
        run(2, "/exportar");
        check_status("Todavía no hay respuestas que exportar.");
        run(3, "un bloque");
        std::vector<Json> got = rpc.take();
        REQUIRE_FALSE(got.empty());
        CHECK(got[0]["result"].dump() == R"({"kind":"message","clear":true})");
        CHECK(summary(got[1]) == "conversation.entry_added 0 user");
        REQUIRE(rpc.finish());
        (void)rpc.take();

        // Cada status de la TUI, con su texto.
        const std::vector<chatbot::cli::CodeBlock> blocks = {[] {
            chatbot::cli::CodeBlock block;
            block.number = 1;
            return block;
        }()};
        run(4, "/copiar tres");
        check_status(std::string{chatbot::cli::kCopyUsage});
        run(5, "/guardar suma.cpp");
        check_status(std::string{chatbot::cli::kSaveUsage});
        run(6, "/exportar todo");
        check_status(std::string{chatbot::cli::kExportUsage});
        run(7, "/copiar 2");
        check_status(chatbot::cli::choose_code_block(blocks, 2).problem);
        run(8, "/guardar 1 ../x");
        check_status(*chatbot::cli::validate_file_name("../x"));
        run(9, "/guardar 1");
        check_status(no_dir);
        run(10, "/exportar");
        check_status(no_dir);

        // copy: el código y el aviso, que también va al historial.
        run(11, "/copiar");
        got = rpc.take();
        CHECK(summaries(got) == std::vector<std::string>{"#11 result",
                                                         "conversation.entry_added 2 notice"});
        CHECK(got[0]["result"].dump() ==
              R"({"kind":"copy","number":1,"language":"cpp","text":"int x;\n",)"
              R"("message":"Copiado el bloque #1 (cpp, 1 línea).","clear":true})");
        CHECK(got[1]["params"]["entry"]["text"] == "Copiado el bloque #1 (cpp, 1 línea).");

        // save: el aviso al historial (entry_added) y la ruta completa.
        rpc.download_base = rpc.path("descargas");
        std::filesystem::create_directory(*rpc.download_base);
        run(12, "/guardar 1 x.cpp");
        got = rpc.take();
        CHECK(summaries(got) == std::vector<std::string>{"#12 result",
                                                         "conversation.entry_added 3 notice"});
        const Json& saved = got[0]["result"];
        CHECK(saved["kind"] == "save");
        CHECK(saved["path"] == (rpc.dir.path() / "descargas" / "chatbot" / "x.cpp").string());
        CHECK(saved["error"] == false);
        CHECK(saved["clear"] == true);
        CHECK(saved["message"] == got[1]["params"]["entry"]["text"]);

        // save que falla al escribir: error al historial, path null, sin vaciar.
        std::ofstream{rpc.dir.path() / "archivo"} << "x";
        rpc.download_base = (rpc.dir.path() / "archivo" / "sub").string();
        run(13, "/guardar 1 y.cpp");
        got = rpc.take();
        CHECK(summaries(got) == std::vector<std::string>{"#13 result",
                                                         "conversation.entry_added 4 error"});
        CHECK(got[0]["result"]["kind"] == "save");
        CHECK(got[0]["result"]["path"].is_null());
        CHECK(got[0]["result"]["error"] == true);
        CHECK(got[0]["result"]["clear"] == false);
        rpc.download_base = rpc.path("descargas");

        run(14, "/exportar");
        got = rpc.take();
        CHECK(summaries(got) == std::vector<std::string>{"#14 result",
                                                         "conversation.entry_added 5 notice"});
        CHECK(got[0]["result"]["kind"] == "export");
        CHECK(got[0]["result"]["error"] == false);
        CHECK(got[0]["result"]["clear"] == true);
        CHECK(got[0]["result"]["path"].get<std::string>().find("conversacion-") !=
              std::string::npos);
    }
    SECTION("search") {
        rpc.env.values["CHAT_SEARCH_API_KEY"] = chatbot_test::kSearchKey;
        rpc.initialize();
        run(2, "/buscar clima");
        const std::vector<Json> got = rpc.take();
        REQUIRE_FALSE(got.empty());
        CHECK(got[0]["result"].dump() == R"({"kind":"search","clear":true})");
        REQUIRE(rpc.finish());
    }
    SECTION("con una respuesta en curso") {
        BlockingTransport* blocking = nullptr;
        rpc.block(blocking);
        rpc.initialize();
        run(2, "hola");
        (void)rpc.take();
        run(3, "otra");
        check_error(only(rpc), -32002, std::string{chatbot::cli::kWaitOrCancel});
        run(4, "/copiar");
        check_status("Espera a que termine la respuesta");
        run(5, "/exportar");
        check_status("Espera a que termine la respuesta");
        rpc.call(6, "chat.cancel", {{"conversation", "c1"}});
        REQUIRE(rpc.finish());
    }
}

TEST_CASE("RPC: blocks.list, blocks.get, blocks.save y conversation.export", "[rpc]") {
    Rpc rpc;
    rpc.responses = {with_block()};
    SECTION("sin respuesta en curso; los botones no emiten eventos") {
        rpc.initialize();
        rpc.call(2, "conversation.export", {{"conversation", "c1"}});
        check_error(only(rpc), -32005, "Todavía no hay respuestas que exportar.");
        rpc.call(3, "blocks.list", {{"conversation", "c1"}});
        CHECK(only(rpc)["result"].dump() == R"({"blocks":[]})");
        exchange(rpc, 4, "c1", "un bloque");

        rpc.call(5, "blocks.list", {{"conversation", "c1"}});
        CHECK(only(rpc)["result"].dump() ==
              R"({"blocks":[{"number":1,"entry":1,"language":"cpp","lines":1,)"
              R"("complete":true,"reason":"none"}]})");
        rpc.call(6, "blocks.get", {{"conversation", "c1"}, {"number", 1}});
        CHECK(only(rpc)["result"].dump() ==
              R"({"code":"int x;\n","language":"cpp","complete":true,"reason":"none"})");
        rpc.call(7, "blocks.get", {{"conversation", "c1"}, {"number", 2}});
        check_error(only(rpc), -32005, "No existe el bloque #2: solo hay el #1.");
        rpc.call(8, "blocks.get", {{"conversation", "c1"}, {"number", 1}, {"file_name", "x"}});
        CHECK(only(rpc)["error"]["code"] == -32602);
        rpc.call(9, "blocks.save", {{"conversation", "c1"}, {"number", 4294967297LL}});
        CHECK(only(rpc)["error"].dump() ==
              R"({"code":-32602,"message":"Parámetros inválidos.","data":{"fields":["number"]}})");

        const std::string no_dir{chatbot::cli::no_download_dir_message(chatbot::current_os())};
        rpc.call(10, "blocks.save", {{"conversation", "c1"}, {"number", 1}});
        check_error(only(rpc), -32007, no_dir);
        rpc.call(11, "conversation.export", {{"conversation", "c1"}});
        check_error(only(rpc), -32007, no_dir);

        rpc.download_base = rpc.path("descargas");
        std::filesystem::create_directory(*rpc.download_base);
        const std::filesystem::path folder = rpc.dir.path() / "descargas" / "chatbot";
        rpc.call(12, "blocks.save", {{"conversation", "c1"}, {"number", 1}});
        Json got = only(rpc); // Sin entry_added: no toca el historial.
        CHECK(got["result"]["path"] == (folder / "bloque-1.cpp").string());
        CHECK(got["result"]["message"] ==
              "Guardado en " + (folder / "bloque-1.cpp").string());
        rpc.call(13, "blocks.save",
                 {{"conversation", "c1"}, {"number", 1}, {"file_name", "suma.cpp"}});
        CHECK(only(rpc)["result"]["path"] == (folder / "suma.cpp").string());
        rpc.call(14, "blocks.save",
                 {{"conversation", "c1"}, {"number", 1}, {"file_name", "../x"}});
        check_error(only(rpc), -32005, *chatbot::cli::validate_file_name("../x"));
        rpc.call(15, "blocks.save", {{"conversation", "c1"}, {"number", 9}});
        check_error(only(rpc), -32005, "No existe el bloque #9: solo hay el #1.");

        rpc.call(16, "conversation.export", {{"conversation", "c1"}});
        got = only(rpc);
        const std::string path = got["result"]["path"];
        CHECK(std::filesystem::exists(path));
        CHECK(got["result"]["message"] == "Conversación exportada a " + path);
        rpc.call(17, "conversation.get", {{"conversation", "c1"}});
        CHECK(only(rpc)["result"]["entries"].size() == 2);

        std::ofstream{rpc.dir.path() / "archivo"} << "x";
        rpc.download_base = (rpc.dir.path() / "archivo" / "sub").string();
        rpc.call(18, "blocks.save", {{"conversation", "c1"}, {"number", 1}});
        CHECK(only(rpc)["error"]["code"] == -32007);
    }
    SECTION("con una respuesta en curso: el bloque a medias, con su aviso") {
        BlockingTransport* blocking = nullptr;
        rpc.chat_transport = [&blocking] {
            auto transport = std::make_unique<BlockingTransport>(BlockingTransport::Mode::Block);
            transport->first_chunk =
                "data: {\"choices\":[{\"delta\":{\"content\":\"```py\\nprint(1)\\n\"}}]}\n\n";
            blocking = transport.get();
            return transport;
        };
        rpc.download_base = rpc.path("descargas");
        std::filesystem::create_directory(*rpc.download_base);
        rpc.initialize();
        rpc.call(2, "chat.send", {{"conversation", "c1"}, {"text", "hola"}});
        (void)rpc.take();
        REQUIRE(rpc.run_until_method("conversation.entry_added"));
        (void)rpc.take();
        rpc.call(3, "blocks.list", {{"conversation", "c1"}});
        CHECK(only(rpc)["result"].dump() ==
              R"({"blocks":[{"number":1,"entry":1,"language":"py","lines":1,)"
              R"("complete":false,"reason":"in_progress"}]})");
        rpc.call(4, "blocks.get", {{"conversation", "c1"}, {"number", 1}});
        CHECK(only(rpc)["result"]["reason"] == "in_progress");
        rpc.call(5, "blocks.save", {{"conversation", "c1"}, {"number", 1}});
        const Json got = only(rpc);
        CHECK(got["result"]["message"].get<std::string>().find(
                  "Ojo: el bloque está incompleto (la respuesta aún no termina).") !=
              std::string::npos);
        rpc.call(6, "chat.cancel", {{"conversation", "c1"}});
        REQUIRE(rpc.finish());
    }
}

namespace {

constexpr const char* kModelKey = "nvapi-clave-ficticia-del-modelo-9876";

/// Borrador de NVIDIA con key y modelo.
Json nvidia_draft(const std::string& model = "m1") {
    return {{"provider", "nvidia"}, {"base_url", ""}, {"model", model}, {"api_key", kModelKey}};
}

chatbot::HttpResponse models_response(int status, std::string body) {
    chatbot::HttpResponse response;
    response.status = status;
    response.body = std::move(body);
    return response;
}

/// Cada models.list responde con response.
void models_reply(Rpc& rpc, const chatbot::HttpResponse& response) {
    rpc.models_transport = [response] {
        auto transport = std::make_unique<chatbot_test::FakeTransport>();
        transport->responses = {response};
        return transport;
    };
}

/// Cada models.list se queda esperando la cancelación; blocking apunta al último.
void models_block(Rpc& rpc, BlockingTransport*& blocking) {
    rpc.models_transport = [&blocking] {
        auto transport = std::make_unique<BlockingTransport>(BlockingTransport::Mode::Block);
        blocking = transport.get();
        return transport;
    };
}

/// Ejecuta tareas hasta que llegue la respuesta a id.
bool run_until_reply(Rpc& rpc, const Json& id) {
    return rpc.queue.run_until([&] {
        for (const Json& message : rpc.out) {
            if (message.contains("id") && message["id"] == id) {
                return true;
            }
        }
        return false;
    });
}

/// Unos instantes de tareas, para ver que no llega nada más.
void settle(Rpc& rpc) {
    (void)rpc.queue.run_until([] { return false; }, std::chrono::milliseconds{150});
}

} // namespace

TEST_CASE("RPC: settings.provider.get", "[rpc][settings]") {
    Rpc rpc;
    SECTION("sin CHAT_*") {
        rpc.config = rpc.load();
        rpc.initialize();
        rpc.call(2, "settings.provider.get");
        const Json result = only(rpc)["result"];
        REQUIRE(result["providers"].size() == chatbot::cli::providers().size());
        CHECK(result["providers"][0]["id"] == std::string{chatbot::cli::providers()[0].id});
        CHECK(result["providers"].back()["id"] == "custom");
        CHECK(result["locks"].dump() == R"({"base_url":false,"model":false,"api_key":false})");
        CHECK(result["saved"].contains("provider"));
        CHECK(result["effective"]["model"] == "");
    }
    SECTION("con CHAT_API_KEY y CHAT_MODEL") {
        rpc.env.values["CHAT_API_KEY"] = kModelKey;
        rpc.env.values["CHAT_MODEL"] = "modelo-del-entorno";
        rpc.config = rpc.load();
        rpc.initialize();
        rpc.call(2, "settings.provider.get");
        const Json result = only(rpc)["result"];
        CHECK(result["locks"].dump() == R"({"base_url":false,"model":true,"api_key":true})");
        CHECK(result["effective"]["model"] == "modelo-del-entorno");
        CHECK(result["key_status"] == "(definido por CHAT_API_KEY) …9876");
        CHECK(result.dump().find(kModelKey) == std::string::npos);
    }
}

TEST_CASE("RPC: settings.provider.check y save", "[rpc][settings]") {
    Rpc rpc;
    rpc.config = rpc.load(); // Sin configuración: ready false.
    rpc.initialize();
    SECTION("check: el error de validate, sin hint") {
        rpc.call(2, "settings.provider.check",
                 {{"draft", {{"provider", "nvidia"}, {"base_url", ""}, {"model", ""}}}});
        const Json result = only(rpc)["result"];
        CHECK(result["error"].is_string());
        CHECK(result["hint"].is_null());
        CHECK(result["can_request_models"] == false);
    }
    SECTION("check: el hint de una URL sin ruta en custom") {
        rpc.call(2, "settings.provider.check",
                 {{"draft",
                   {{"provider", "custom"}, {"base_url", "https://mi-servidor"}, {"model", "m"},
                    {"api_key", kModelKey}}}});
        const Json result = only(rpc)["result"];
        CHECK(result["error"].is_null());
        CHECK(result["hint"].is_string());
        CHECK(result["can_request_models"] == true);
        CHECK(result["dirty"] == true);
    }
    SECTION("save: ready, settings.changed y el aviso al historial") {
        rpc.call(2, "settings.provider.save", {{"draft", nvidia_draft()}});
        const std::vector<Json> got = rpc.take();
        CHECK(summaries(got) == std::vector<std::string>{"#2 result", "settings.changed provider",
                                                         "conversation.entry_added 0 notice"});
        CHECK(got[0]["result"].dump() == R"({"ready":true})");
        CHECK(got[2]["params"]["entry"]["text"] == "Ahora se usa m1 de NVIDIA NIM.");
        CHECK(rpc.clients == 1);
        rpc.call(3, "settings.provider.get");
        const Json result = only(rpc)["result"];
        CHECK(result["saved"]["provider"] == "nvidia");
        CHECK(result["effective"]["model"] == "m1");
        CHECK(result["key_status"] == "guardada: …9876");
    }
    SECTION("save inválido: -32005 sin escribir") {
        rpc.call(2, "settings.provider.save",
                 {{"draft", {{"provider", "nvidia"}, {"base_url", ""}, {"model", ""}}}});
        CHECK(only(rpc)["error"]["code"] == -32005);
        CHECK_FALSE(std::filesystem::exists(rpc.config_path()));
    }
    SECTION("un provider que no está en la tabla, o un campo de menos: -32602") {
        Json draft = nvidia_draft();
        draft["provider"] = "otro";
        rpc.call(2, "settings.provider.check", {{"draft", draft}});
        CHECK(only(rpc)["error"]["data"].dump() == R"({"fields":["draft.provider"]})");
        draft = nvidia_draft();
        draft.erase("model");
        draft["sobra"] = 1;
        rpc.call(3, "models.list", {{"draft", draft}});
        CHECK(only(rpc)["error"]["data"].dump() == R"({"fields":["draft.sobra","draft.model"]})");
    }
}

TEST_CASE("RPC: los guardados con una respuesta en curso → Busy; gui sí", "[rpc][settings]") {
    Rpc rpc;
    BlockingTransport* blocking = nullptr;
    rpc.block(blocking);
    rpc.initialize();
    rpc.call(2, "chat.send", {{"conversation", "c1"}, {"text", "hola"}});
    REQUIRE(blocking->started.wait_for(kThreadTimeout));
    (void)rpc.take();
    rpc.call(3, "settings.provider.save", {{"draft", nvidia_draft()}});
    check_error(only(rpc), -32002, std::string{chatbot::cli::kWaitOrCancel});
    rpc.call(4, "settings.system_prompt.save", {{"text", ""}});
    check_error(only(rpc), -32002, std::string{chatbot::cli::kWaitOrCancel});
    rpc.call(5, "settings.search.save", {{"api_key", chatbot_test::kSearchKey}});
    check_error(only(rpc), -32002, std::string{chatbot::cli::kWaitOrCancel});
    CHECK_FALSE(std::filesystem::exists(rpc.config_path()));
    CHECK_FALSE(std::filesystem::exists(rpc.credentials_path()));
    rpc.call(6, "settings.gui.set", {{"value", {{"tema", "oscuro"}}}});
    CHECK(summaries(rpc.take()) == std::vector<std::string>{"#6 result", "settings.changed gui"});
    rpc.call(7, "chat.cancel", {{"conversation", "c1"}});
    REQUIRE(rpc.finish());
}

TEST_CASE("RPC: models.list", "[rpc][settings]") {
    Rpc rpc;
    SECTION("bien") {
        models_reply(rpc, models_response(200, R"({"data":[{"id":"b"},{"id":"a"}]})"));
        rpc.initialize();
        rpc.call(2, "models.list", {{"draft", nvidia_draft()}});
        CHECK(rpc.out.empty()); // Sale después, por post.
        REQUIRE(run_until_reply(rpc, 2));
        CHECK(only(rpc)["result"].dump() == R"({"models":["a","b"]})");
    }
    SECTION("falla del servidor: -32005 con el texto de F2") {
        const chatbot::HttpResponse denied =
            models_response(401, R"({"error":{"message":"La key no sirve"}})");
        models_reply(rpc, denied);
        rpc.initialize();
        rpc.call(2, "models.list", {{"draft", nvidia_draft()}});
        REQUIRE(run_until_reply(rpc, 2));
        const Json got = only(rpc);
        CHECK(got["error"]["code"] == -32005);
        CHECK(got["error"]["data"]["message"].get<std::string>().starts_with(
            "[error de autenticación] "));
    }
    SECTION("sin URL ni key: -32005 sin pedir nada") {
        rpc.initialize();
        rpc.call(2, "models.list",
                 {{"draft", {{"provider", "nvidia"}, {"base_url", ""}, {"model", ""}}}});
        check_error(only(rpc), -32005, std::string{chatbot::cli::kModelsNeedUrlAndKey});
    }
    SECTION("$/cancel: -32800 al momento y nada después") {
        BlockingTransport* blocking = nullptr;
        models_block(rpc, blocking);
        rpc.initialize();
        rpc.call(2, "models.list", {{"draft", nvidia_draft()}});
        REQUIRE(blocking->started.wait_for(kThreadTimeout));
        rpc.notify("$/cancel", {{"id", 99}}); // Otro id: nada.
        CHECK(rpc.out.empty());
        rpc.call(3, "$/cancel", {{"id", 2}});
        CHECK(summaries(rpc.take()) == std::vector<std::string>{"#2 error -32800", "#3 result"});
        settle(rpc);
        CHECK(rpc.out.empty());
    }
    SECTION("otro models.list reemplaza al pendiente, que responde -32800") {
        BlockingTransport* blocking = nullptr;
        models_block(rpc, blocking);
        rpc.initialize();
        rpc.call("a", "models.list", {{"draft", nvidia_draft()}});
        REQUIRE(blocking->started.wait_for(kThreadTimeout));
        models_reply(rpc, models_response(200, R"({"data":[{"id":"x"}]})"));
        rpc.call("b", "models.list", {{"draft", nvidia_draft()}});
        CHECK(summaries(rpc.take()) == std::vector<std::string>{R"(#"a" error -32800)"});
        REQUIRE(run_until_reply(rpc, "b"));
        CHECK(only(rpc)["result"].dump() == R"({"models":["x"]})");
        settle(rpc);
        CHECK(rpc.out.empty());
    }
    SECTION("pendiente al shutdown: sin respuesta") {
        BlockingTransport* blocking = nullptr;
        models_block(rpc, blocking);
        rpc.initialize();
        rpc.call(2, "models.list", {{"draft", nvidia_draft()}});
        REQUIRE(blocking->started.wait_for(kThreadTimeout));
        rpc.call(3, "shutdown");
        CHECK(summaries(rpc.take()) ==
              std::vector<std::string>{"conversation.closed c1 shutdown", "#3 result"});
        settle(rpc);
        CHECK(rpc.out.empty());
    }
}

TEST_CASE("RPC: settings.system_prompt.*", "[rpc][settings]") {
    Rpc rpc;
    rpc.initialize();
    const std::string default_text{chatbot::kDefaultSystemPrompt};
    rpc.call(2, "settings.system_prompt.get");
    CHECK(only(rpc)["result"].dump() ==
          Json({{"mode", "default"},
                {"text", default_text},
                {"default_text", default_text},
                {"max_bytes", chatbot::kMaxSystemPromptBytes},
                {"warning", nullptr}})
              .dump());

    rpc.call(3, "settings.system_prompt.save", {{"text", ""}});
    std::vector<Json> got = rpc.take();
    CHECK(summaries(got) == std::vector<std::string>{"#3 result", "settings.changed system_prompt",
                                                     "conversation.entry_added 0 notice"});
    CHECK(got[0]["result"].dump() == R"({"mode":"none"})");
    CHECK(got[2]["params"]["entry"]["text"] ==
          "Sin instrucciones del sistema desde el siguiente mensaje.");
    rpc.call(4, "settings.system_prompt.get");
    CHECK(only(rpc)["result"]["mode"] == "none");

    rpc.call(5, "settings.system_prompt.save", {{"text", "Eres un tutor."}});
    CHECK(rpc.take()[0]["result"].dump() == R"({"mode":"custom"})");
    rpc.call(6, "settings.system_prompt.get");
    const Json custom = only(rpc)["result"];
    CHECK(custom["mode"] == "custom");
    CHECK(custom["text"] == "Eres un tutor.");

    // Restaurar: mandar default_text borra la llave.
    rpc.call(7, "settings.system_prompt.save", {{"text", default_text}});
    CHECK(rpc.take()[0]["result"].dump() == R"({"mode":"default"})");
    CHECK(chatbot::load_system_prompt_value(rpc.config_path()).value() == std::nullopt);

    rpc.call(8, "settings.system_prompt.check", {{"text", "ñ"}});
    CHECK(only(rpc)["result"].dump() == R"({"error":null,"warning":null,"bytes":2})");
    const std::string bad = "a\x01" "b";
    rpc.call(9, "settings.system_prompt.check", {{"text", bad}});
    CHECK(only(rpc)["result"]["error"] == chatbot::validate_system_prompt(bad).value());
    rpc.call(10, "settings.system_prompt.save", {{"text", bad}});
    check_error(only(rpc), -32005, chatbot::validate_system_prompt(bad).value());
    rpc.call(11, "settings.system_prompt.save", {{"text", nullptr}});
    CHECK(only(rpc)["error"]["code"] == -32602);
}

TEST_CASE("RPC: settings.search.*", "[rpc][settings]") {
    Rpc rpc;
    SECTION("get y save") {
        rpc.initialize();
        rpc.call(2, "settings.search.get");
        CHECK(only(rpc)["result"].dump() ==
              R"({"configured":false,"locked":false,"key_status":"sin configurar"})");
        rpc.call(3, "settings.search.save", {{"api_key", "$TAVILY"}});
        CHECK(only(rpc)["error"]["code"] == -32005);
        rpc.call(4, "settings.search.save", {{"api_key", ""}});
        check_error(only(rpc), -32005, "Escribe la key de búsqueda.");
        rpc.call(5, "settings.search.save", {{"api_key", chatbot_test::kSearchKey}});
        CHECK(summaries(rpc.take()) == std::vector<std::string>{"#5 result",
                                                                "settings.changed search",
                                                                "conversation.entry_added 0 notice"});
        rpc.call(6, "app.status");
        CHECK(only(rpc)["result"]["has_search_key"] == true);
        rpc.call(7, "settings.search.get");
        CHECK(only(rpc)["result"]["configured"] == true);
    }
    SECTION("con CHAT_SEARCH_API_KEY: locked, y save → -32005") {
        rpc.env.values["CHAT_SEARCH_API_KEY"] = chatbot_test::kSearchKey;
        rpc.initialize();
        rpc.call(2, "settings.search.get");
        const Json result = only(rpc)["result"];
        CHECK(result["locked"] == true);
        CHECK(result["configured"] == true);
        rpc.call(3, "settings.search.save", {{"api_key", "tvly-otra-clave-ficticia-de-busqueda"}});
        CHECK(only(rpc)["error"]["code"] == -32005);
        CHECK_FALSE(std::filesystem::exists(rpc.credentials_path()));
    }
}

TEST_CASE("RPC: settings.gui.*", "[rpc][settings]") {
    Rpc rpc;
    rpc.initialize();
    rpc.call(2, "settings.gui.get");
    CHECK(only(rpc)["result"].dump() == R"({"value":null})");
    rpc.call(3, "settings.gui.set", {{"value", {{"tema", "oscuro"}, {"ancho", 280}}}});
    CHECK(summaries(rpc.take()) == std::vector<std::string>{"#3 result", "settings.changed gui"});
    rpc.call(4, "settings.gui.get");
    CHECK(only(rpc)["result"].dump() == R"({"value":{"tema":"oscuro","ancho":280}})");
    rpc.call(5, "settings.gui.set", {{"value", nullptr}});
    (void)rpc.take();
    rpc.call(6, "settings.gui.get");
    CHECK(only(rpc)["result"].dump() == R"({"value":null})");

    rpc.call(7, "settings.gui.set", {{"value", 5}});
    CHECK(only(rpc)["error"]["code"] == -32005);
    rpc.call(8, "settings.gui.set", {{"value", Json::array()}});
    CHECK(only(rpc)["error"]["code"] == -32005);
    rpc.call(9, "settings.gui.set",
             {{"value", {{"x", std::string(chatbot::cli::kMaxGuiSettingsBytes, 'a')}}}});
    CHECK(only(rpc)["error"]["code"] == -32005);
    rpc.call(10, "settings.gui.set");
    CHECK(only(rpc)["error"]["data"].dump() == R"({"fields":["value"]})");
}

TEST_CASE("RPC: ninguna respuesta ni evento trae la key que se mandó", "[rpc][settings]") {
    Rpc rpc;
    models_reply(rpc, models_response(401, R"({"error":{"message":"no"}})"));
    rpc.config = rpc.load();
    rpc.initialize();
    rpc.call(2, "settings.provider.check", {{"draft", nvidia_draft()}});
    rpc.call(3, "models.list", {{"draft", nvidia_draft()}});
    REQUIRE(run_until_reply(rpc, 3));
    rpc.call(4, "settings.provider.save", {{"draft", nvidia_draft()}});
    rpc.call(5, "settings.provider.get");
    rpc.call(6, "settings.search.save", {{"api_key", chatbot_test::kSearchKey}});
    rpc.call(7, "settings.search.get");
    rpc.call(8, "app.status");
    std::string all;
    for (const Json& message : rpc.take()) {
        all += message.dump() + "\n";
    }
    CHECK(all.find(kModelKey) == std::string::npos);
    CHECK(all.find(chatbot_test::kSearchKey) == std::string::npos);
    CHECK(all.find("\"id\":8") != std::string::npos);
}

