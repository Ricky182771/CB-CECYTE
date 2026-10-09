#include "conversation.h"
#include "conversation_export.h"

#include "chatbot/error.h"
#include "chatbot/web_search.h"

#include <catch2/catch_test_macros.hpp>

#include <ctime>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>

namespace {

using chatbot::cli::Conversation;

std::string read_data(const std::string& name) {
    std::ifstream file(std::string{CHATBOT_TEST_DATA_DIR} + "/" + name, std::ios::binary);
    REQUIRE(file.good());
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

void answer(Conversation& conversation, const std::string& text, const std::string& model) {
    conversation.append_delta(text);
    REQUIRE_FALSE(conversation.finish_success("stop", model).has_value());
}

/// Conversación de ejemplo: un par normal, un /buscar con una fuente con
/// fecha y otra sin ella, un par que falló (no se exporta), un aviso y un
/// par con otro modelo. Las instrucciones de sistema y el bloque de
/// resultados llevan marcas que no deben aparecer en el archivo.
Conversation sample() {
    Conversation conversation{"INSTRUCCIONES-SECRETAS"};
    REQUIRE(conversation.submit("¿Cómo sumo dos números en C++?").has_value());
    answer(conversation,
           "Así:\n\n```cpp\nint suma(int a, int b) {\n    return a + b;\n}\n```\n",
           "meta/llama-3.3-70b-instruct");

    REQUIRE(conversation.submit("/buscar clima en Monterrey").has_value());
    chatbot::SearchResponse response;
    response.query = "clima en Monterrey";
    response.results = {
        {"Pronóstico [Monterrey]", "https://clima.example.mx/mty", "contenido uno", "2026-10-04"},
        {"Sin fecha", "https://otro.example.com/a b(1)", "contenido dos", ""},
    };
    REQUIRE(conversation.attach_search(response, "2026-10-05",
                                       "<resultados id=\"BLOQUE-SECRETO\">…</resultados>"));
    answer(conversation, "Hace calor [1].", "meta/llama-3.3-70b-instruct");

    REQUIRE(conversation.submit("esto falla").has_value());
    (void)conversation.finish_error(
        chatbot::ChatError{chatbot::ErrorKind::Server, 500, "ERROR-SECRETO", std::nullopt});
    conversation.add_notice("AVISO-SECRETO");

    REQUIRE(conversation.submit("gracias").has_value());
    answer(conversation, "¡De nada!", "qwen/qwen3-coder");
    conversation.set_identity("20261005-093000-abc123", "2026-10-05T09:30:00-06:00");
    return conversation;
}

} // namespace

TEST_CASE("exportar: la conversación de ejemplo coincide con el archivo esperado",
          "[exportar]") {
    const Conversation conversation = sample();
    const std::string markdown = chatbot::cli::export_markdown(
        conversation.to_stored("2026-10-05T10:00:00-06:00"), "2026-10-05 10:00");
    CHECK(markdown == read_data("conversacion_exportada.md"));
    // Nunca las instrucciones, el bloque de resultados, errores ni avisos.
    for (const char* secret :
         {"INSTRUCCIONES-SECRETAS", "BLOQUE-SECRETO", "<resultados", "ERROR-SECRETO",
          "AVISO-SECRETO", "esto falla", "contenido uno"}) {
        INFO(secret);
        CHECK(markdown.find(secret) == std::string::npos);
    }
}

TEST_CASE("exportar: URL que no es web va como texto plano", "[exportar]") {
    chatbot::cli::StoredConversation stored;
    stored.title = "Fuentes raras";
    chatbot::cli::StoredMessage user;
    user.role = chatbot::Role::User;
    user.content = "/buscar algo";
    chatbot::cli::StoredSearch search;
    search.date = "2026-10-05";
    search.response.results = {
        {"Malicioso", "javascript:alert(1)", "x", "2026-01-02"},
        {"", "file:///etc/passwd", "x", ""},
        {"", "https://sin-titulo.example.com", "x", ""},
    };
    user.search = search;
    chatbot::cli::StoredMessage assistant;
    assistant.role = chatbot::Role::Assistant;
    assistant.content = "Respuesta";
    stored.messages = {user, assistant};

    const std::string markdown = chatbot::cli::export_markdown(stored, "2026-10-05 10:00");
    CHECK(markdown == "# Fuentes raras\n\n2026-10-05 10:00\n\n## Tú\n\n/buscar algo\n\n"
                      "## Asistente\n\nRespuesta\n\n### Fuentes\n\n"
                      "1. Malicioso — 2026-01-02 (javascript:alert(1))\n"
                      "2. file:///etc/passwd\n"
                      "3. [https://sin-titulo.example.com](https://sin-titulo.example.com)\n");
    CHECK(markdown.find("](javascript") == std::string::npos);
    CHECK(markdown.find("](file") == std::string::npos);
}

TEST_CASE("exportar: sin título usa uno genérico", "[exportar]") {
    const chatbot::cli::StoredConversation empty;
    CHECK(chatbot::cli::export_markdown(empty, "2026-10-05 10:00") ==
          "# Conversación\n\n2026-10-05 10:00\n");
}

TEST_CASE("exportar: fecha y nombre del archivo", "[exportar]") {
    CHECK(chatbot::cli::export_date("2026-10-02T23:58:00-06:00") == "2026-10-02 23:58");
    CHECK(chatbot::cli::export_date("ayer") == "ayer");
    CHECK(chatbot::cli::export_file_name("20261005-093000-abc123", 0) ==
          "conversacion-20261005-093000-abc123.md");
    // Sin id, la fecha y hora local.
    std::tm local{};
    local.tm_year = 2026 - 1900;
    local.tm_mon = 9;
    local.tm_mday = 8;
    local.tm_hour = 14;
    local.tm_min = 5;
    local.tm_sec = 9;
    local.tm_isdst = -1;
    const std::time_t when = std::mktime(&local);
    CHECK(chatbot::cli::export_file_name("", when) == "conversacion-20261008-140509.md");
}
