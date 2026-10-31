// Pruebas puras de chatbot-rpc: LineReader, diff_entries, codificación al
// protocolo, check_params y la redacción de CHAT_DEBUG_RPC.

#include "debug_log.h"
#include "entry_diff.h"
#include "line_reader.h"
#include "protocol.h"

#include "conversation.h"

#include "temp_dir.hpp"

#include "chatbot/error.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

using chatbot::rpc::Json;
using chatbot::rpc::LineReader;
using Kind = LineReader::Item::Kind;

std::vector<std::string> lines_of(const std::vector<LineReader::Item>& items) {
    std::vector<std::string> lines;
    for (const LineReader::Item& item : items) {
        lines.push_back(item.kind == Kind::Line ? item.text : "<oversized>");
    }
    return lines;
}

} // namespace

TEST_CASE("LineReader corta líneas sin importar cómo lleguen los bloques", "[rpc]") {
    LineReader reader;
    SECTION("una línea partida en varios bloques") {
        CHECK(reader.feed("{\"a\"").empty());
        CHECK(reader.feed(":1").empty());
        CHECK(lines_of(reader.feed("}\n")) == std::vector<std::string>{"{\"a\":1}"});
    }
    SECTION("varias líneas en un bloque, \\r\\n tolerado y líneas vacías ignoradas") {
        CHECK(lines_of(reader.feed("uno\r\n\n\r\ndos\ntr")) ==
              std::vector<std::string>{"uno", "dos"});
        CHECK(lines_of(reader.feed("es\n")) == std::vector<std::string>{"tres"});
    }
    SECTION("EOF con una línea sin \\n") {
        CHECK(reader.feed("sin salto").empty());
        const auto last = reader.finish();
        REQUIRE(last.has_value());
        CHECK(last->text == "sin salto");
        CHECK_FALSE(reader.finish().has_value());
    }
}

TEST_CASE("LineReader descarta una línea de más del tope hasta el siguiente \\n", "[rpc]") {
    LineReader reader{8};
    // Exactamente el tope pasa.
    CHECK(lines_of(reader.feed("12345678\n")) == std::vector<std::string>{"12345678"});
    // Se avisa una sola vez, aunque siga llegando en otros bloques.
    CHECK(lines_of(reader.feed("123456789")) == std::vector<std::string>{"<oversized>"});
    CHECK(reader.feed("mucho más texto").empty());
    CHECK(lines_of(reader.feed("fin\nsigue\n")) == std::vector<std::string>{"sigue"});
    // Una línea de más que llega al EOF tampoco se entrega.
    CHECK(lines_of(reader.feed("abcdefghij")) == std::vector<std::string>{"<oversized>"});
    CHECK_FALSE(reader.finish().has_value());
}

TEST_CASE("LineReader con el tope real de 1 MiB", "[rpc]") {
    LineReader reader;
    const std::string big(chatbot::rpc::kMaxLineBytes + 1, 'x');
    CHECK(lines_of(reader.feed(big + "\n{}\n")) == std::vector<std::string>{"<oversized>", "{}"});
}

namespace {

using chatbot::cli::Conversation;
using chatbot::cli::Entry;
using chatbot::cli::EntryKind;

} // namespace

TEST_CASE("diff_entries", "[rpc]") {
    Conversation conversation{""};
    chatbot::rpc::EntriesSeen seen = chatbot::rpc::remember(conversation.entries());

    SECTION("nada cambió") {
        const auto diff = chatbot::rpc::diff_entries(seen, conversation.entries());
        CHECK_FALSE(diff.grown);
        CHECK_FALSE(diff.updated);
        CHECK_FALSE(diff.added(seen));
    }
    REQUIRE(conversation.submit("hola").has_value());
    SECTION("entrada agregada") {
        const auto diff = chatbot::rpc::diff_entries(seen, conversation.entries());
        CHECK(diff.added(seen));
        CHECK(diff.added_end == 1);
        CHECK_FALSE(diff.grown);
    }
    seen = chatbot::rpc::remember(conversation.entries());
    conversation.append_delta("Ho");
    seen = chatbot::rpc::remember(conversation.entries());
    SECTION("texto agregado a la respuesta en curso") {
        conversation.append_delta("la");
        const auto diff = chatbot::rpc::diff_entries(seen, conversation.entries());
        CHECK(diff.grown);
        CHECK_FALSE(diff.updated);
        CHECK_FALSE(diff.added(seen));
        CHECK(conversation.entries()[seen.count - 1].text.substr(seen.last_text_size) == "la");
    }
    SECTION("marcas cambiadas al terminar (y la nota)") {
        REQUIRE_FALSE(conversation.finish_success("length", "m").has_value());
        const auto diff = chatbot::rpc::diff_entries(seen, conversation.entries());
        CHECK(diff.updated);
        CHECK_FALSE(diff.grown);
        CHECK_FALSE(diff.added(seen));
    }
    SECTION("finish_error: marca la respuesta e inserta el error") {
        (void)conversation.finish_error(
            chatbot::ChatError{chatbot::ErrorKind::Server, 500, "caído", std::nullopt});
        const auto diff = chatbot::rpc::diff_entries(seen, conversation.entries());
        CHECK(diff.updated);
        CHECK(diff.added(seen));
        CHECK(diff.added_end == seen.count + 1);
        CHECK(conversation.entries()[seen.count].kind == EntryKind::Error);
        CHECK(conversation.entries()[seen.count - 1].incomplete);
    }
}

TEST_CASE("Codificación de Entry", "[rpc]") {
    Entry entry;
    entry.kind = EntryKind::Assistant;
    entry.text = "¡Hola!";
    entry.in_progress = true;
    entry.note = "nota";
    CHECK(chatbot::rpc::encode_entry(entry).dump() ==
          R"({"kind":"assistant","text":"¡Hola!","in_progress":true,"incomplete":false,)"
          R"("cancelled":false,"note":"nota"})");

    Entry sources;
    sources.kind = EntryKind::Sources;
    sources.sources = {{"Uno", "https://uno.mx", "c", "2026-10-20"},
                       {"Dos", "https://dos.mx", "c", ""}};
    const Json encoded = chatbot::rpc::encode_entry(sources);
    CHECK(encoded["kind"] == "sources");
    CHECK(encoded["sources"].dump() ==
          R"([{"n":1,"title":"Uno","url":"https://uno.mx","published_date":"2026-10-20"},)"
          R"({"n":2,"title":"Dos","url":"https://dos.mx","published_date":null}])");
    // Las demás entradas no llevan "sources".
    CHECK_FALSE(chatbot::rpc::encode_entry(entry).contains("sources"));
}

TEST_CASE("Codificación de cada ErrorKind y de ChatError", "[rpc]") {
    using chatbot::ErrorKind;
    const std::vector<std::pair<ErrorKind, std::string>> names{
        {ErrorKind::Config, "config"},
        {ErrorKind::Auth, "auth"},
        {ErrorKind::ModelNotFound, "model_not_found"},
        {ErrorKind::RateLimited, "rate_limited"},
        {ErrorKind::Server, "server"},
        {ErrorKind::Network, "network"},
        {ErrorKind::Timeout, "timeout"},
        {ErrorKind::BadResponse, "bad_response"},
        {ErrorKind::InvalidRequest, "invalid_request"},
        {ErrorKind::Cancelled, "cancelled"},
        {ErrorKind::NoSearchResults, "no_search_results"},
    };
    for (const auto& [kind, name] : names) {
        CHECK(chatbot::rpc::error_kind_name(kind) == name);
    }
    CHECK(chatbot::rpc::encode_error({ErrorKind::RateLimited, 429, "despacio",
                                      std::chrono::seconds{20}})
              .dump() ==
          R"({"kind":"rate_limited","http_status":429,"message":"despacio","retry_after_seconds":20})");
    CHECK(chatbot::rpc::encode_error({ErrorKind::Network, 0, "sin red", std::nullopt})
              .dump() ==
          R"({"kind":"network","http_status":0,"message":"sin red","retry_after_seconds":null})");
}

TEST_CASE("Errores de JSON-RPC", "[rpc]") {
    using chatbot::rpc::RpcError;
    CHECK(chatbot::rpc::error_response(7, RpcError::MethodNotFound).dump() ==
          R"({"jsonrpc":"2.0","id":7,"error":{"code":-32601,"message":"Método desconocido."}})");
    CHECK(chatbot::rpc::error_response(nullptr, RpcError::ParseError)["error"]["code"] == -32700);
    CHECK(chatbot::rpc::error_response("a", RpcError::Busy, chatbot::rpc::message_data("espera"))
              .dump() ==
          R"({"jsonrpc":"2.0","id":"a","error":{"code":-32002,"message":"Hay una respuesta en curso.",)"
          R"("data":{"message":"espera"}}})");
    const std::vector<std::pair<RpcError, int>> codes{
        {RpcError::InvalidRequest, -32600},   {RpcError::InvalidParams, -32602},
        {RpcError::InternalError, -32603},    {RpcError::RequestCancelled, -32800},
        {RpcError::NotInitialized, -32001},   {RpcError::UnknownConversation, -32003},
        {RpcError::NotConfigured, -32004},    {RpcError::ValidationFailed, -32005},
        {RpcError::Unsupported, -32006},      {RpcError::StorageError, -32007},
        {RpcError::ConfirmationRequired, -32008}, {RpcError::ProtocolMismatch, -32010},
        {RpcError::StartupFailed, -32011}};
    for (const auto& [code, number] : codes) {
        const Json error = chatbot::rpc::error_object(code);
        CHECK(error["code"] == number);
        CHECK_FALSE(error["message"].get<std::string>().empty());
        CHECK_FALSE(error.contains("data"));
    }
}

TEST_CASE("to_line: una línea, sin \\n dentro y con UTF-8 inválido sustituido", "[rpc]") {
    const std::string line =
        chatbot::rpc::to_line(Json{{"text", std::string{"a\nb\xff"}}});
    CHECK(line == "{\"text\":\"a\\nb\xEF\xBF\xBD\"}\n");
}

TEST_CASE("check_params", "[rpc]") {
    using chatbot::rpc::Param;
    const Json params{{"conversation", "c1"}, {"text", 5}, {"extra", true}};
    const auto bad = chatbot::rpc::check_params(
        &params, {{"conversation"}, {"text"}, {"search", Param::Type::String, false}});
    REQUIRE(bad.has_value());
    CHECK((*bad)["fields"] == Json::array({"extra", "text"}));
    CHECK_FALSE(chatbot::rpc::check_params(nullptr, {}).has_value());
    const auto missing = chatbot::rpc::check_params(nullptr, {{"conversation"}});
    REQUIRE(missing.has_value());
    CHECK((*missing)["fields"] == Json::array({"conversation"}));
    const Json array = Json::array();
    CHECK(chatbot::rpc::check_params(&array, {}).has_value());
}

TEST_CASE("CHAT_DEBUG_RPC tapa los secretos a cualquier profundidad", "[rpc]") {
    const Json message = Json::parse(R"({"params":{"draft":{"api_key":"nvapi-secreta",
        "model":"m"},"list":[{"token":"123:ABC"},{"model_key":"k1","search_key":"k2"}],
        "text":"api_key no es una llave aquí"}})");
    const Json redacted = chatbot::rpc::redact_secrets(message);
    const std::string text = redacted.dump();
    for (const char* secret : {"nvapi-secreta", "123:ABC", "\"k1\"", "\"k2\""}) {
        CHECK(text.find(secret) == std::string::npos);
    }
    CHECK(redacted["params"]["draft"]["api_key"] == chatbot::rpc::kRedacted);
    CHECK(redacted["params"]["draft"]["model"] == "m");
    CHECK(redacted["params"]["text"] == "api_key no es una llave aquí");
    // addons.telegram.save: los secretos y el código, dentro de form.fields.
    const Json save = chatbot::rpc::redact_secrets(Json::parse(
        R"({"params":{"form":{"fields":{"token":"123:ABC","invite_code":"codigo-secreto",)"
        R"("model":"m"}}}})"));
    CHECK(save["params"]["form"]["fields"]["token"] == chatbot::rpc::kRedacted);
    CHECK(save["params"]["form"]["fields"]["invite_code"] == chatbot::rpc::kRedacted);
    CHECK(save["params"]["form"]["fields"]["model"] == "m");

    // El archivo: entra tapado; lo que no es JSON solo con su tamaño.
    const chatbot_test::ScopedTempDir dir;
    const std::string path = (dir.path() / "rpc.log").string();
    {
        chatbot::rpc::DebugLog log{path};
        log.incoming(message.dump());
        log.incoming("{\"api_key\": \"nvapi-rota");
        log.outgoing(Json{{"result", {{"token", "123:ABC"}}}});
    }
    std::ifstream file{path};
    const std::string written{std::istreambuf_iterator<char>{file}, {}};
    CHECK(written.find("nvapi") == std::string::npos);
    CHECK(written.find("123:ABC") == std::string::npos);
    CHECK(written.find("→ (no es JSON, 23 bytes)") != std::string::npos);
    CHECK(written.find("← {\"result\":{\"token\":\"•••\"}}") != std::string::npos);
}
