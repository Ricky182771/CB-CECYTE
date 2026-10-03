#include "conversation.h"

#include "chatbot/error.h"
#include "chatbot/types.h"

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <vector>

namespace {

using chatbot::Role;
using chatbot::cli::Conversation;
using chatbot::cli::Entry;
using chatbot::cli::EntryKind;

chatbot::ChatError sample_error() {
    return chatbot::ChatError{chatbot::ErrorKind::ModelNotFound, 404,
                              "El modelo no existe.", std::nullopt};
}

/// Ningún mensaje del historial para la API puede estar vacío.
void check_no_empty_messages(const Conversation& conversation) {
    for (const chatbot::Message& message : conversation.history()) {
        CHECK_FALSE(message.content.empty());
    }
}

} // namespace

TEST_CASE("Conversation: empieza con el prompt de sistema", "[conversacion]") {
    const Conversation conversation;
    REQUIRE(conversation.history().size() == 1);
    CHECK(conversation.history()[0].role == Role::System);
    CHECK(conversation.history()[0].content == chatbot::cli::kSystemPrompt);
    CHECK(conversation.entries().empty());
    CHECK_FALSE(conversation.busy());
}

TEST_CASE("Conversation: submit vacío o de solo espacios no hace nada", "[conversacion]") {
    Conversation conversation;
    CHECK_FALSE(conversation.submit("").has_value());
    CHECK_FALSE(conversation.submit("   \t  ").has_value());
    CHECK(conversation.entries().empty());
    CHECK(conversation.history().size() == 1);
    CHECK_FALSE(conversation.busy());
}

TEST_CASE("Conversation: submit recorta espacios y devuelve una copia del historial",
          "[conversacion]") {
    Conversation conversation;
    const std::optional<std::vector<chatbot::Message>> to_send =
        conversation.submit("  Hola  ");
    REQUIRE(to_send.has_value());
    REQUIRE(to_send->size() == 2);
    CHECK((*to_send)[1].role == Role::User);
    CHECK((*to_send)[1].content == "Hola");
    CHECK(conversation.busy());
    REQUIRE(conversation.entries().size() == 1);
    CHECK(conversation.entries()[0].kind == EntryKind::User);
    CHECK(conversation.entries()[0].text == "Hola");
}

TEST_CASE("Conversation: submit mientras está ocupado no hace nada", "[conversacion]") {
    Conversation conversation;
    REQUIRE(conversation.submit("Primero").has_value());
    CHECK_FALSE(conversation.submit("Segundo").has_value());
    CHECK(conversation.entries().size() == 1);
    CHECK(conversation.history().size() == 2);
}

TEST_CASE("Conversation: flujo exitoso con varios deltas", "[conversacion]") {
    Conversation conversation;
    REQUIRE(conversation.submit("Hola").has_value());
    conversation.append_delta("Hola, ");
    REQUIRE(conversation.entries().size() == 2);
    CHECK(conversation.entries()[1].kind == EntryKind::Assistant);
    CHECK(conversation.entries()[1].in_progress);
    conversation.append_delta("¿en qué ");
    conversation.append_delta("te ayudo?");
    REQUIRE(conversation.entries().size() == 2);
    CHECK(conversation.entries()[1].text == "Hola, ¿en qué te ayudo?");

    CHECK_FALSE(conversation.finish_success().has_value());
    CHECK_FALSE(conversation.busy());
    CHECK_FALSE(conversation.entries()[1].in_progress);
    CHECK_FALSE(conversation.entries()[1].incomplete);
    REQUIRE(conversation.history().size() == 3);
    CHECK(conversation.history()[2].role == Role::Assistant);
    CHECK(conversation.history()[2].content == "Hola, ¿en qué te ayudo?");
}

TEST_CASE("Conversation: éxito sin texto se trata como error", "[conversacion]") {
    Conversation conversation;
    REQUIRE(conversation.submit("Hola").has_value());

    const std::optional<std::string> restored = conversation.finish_success();
    REQUIRE(restored.has_value());
    CHECK(*restored == "Hola");
    CHECK_FALSE(conversation.busy());
    // El mensaje de usuario sale del historial, pero sigue en pantalla.
    CHECK(conversation.history().size() == 1);
    REQUIRE(conversation.entries().size() == 2);
    CHECK(conversation.entries()[0].kind == EntryKind::User);
    CHECK(conversation.entries()[1].kind == EntryKind::Error);
    CHECK(conversation.entries()[1].text ==
          "[respuesta inválida] El modelo no devolvió texto.");
    check_no_empty_messages(conversation);
}

TEST_CASE("Conversation: error sin texto parcial", "[conversacion]") {
    Conversation conversation;
    REQUIRE(conversation.submit("Hola").has_value());

    (void)conversation.finish_error(sample_error());
    CHECK_FALSE(conversation.busy());
    CHECK(conversation.history().size() == 1);
    REQUIRE(conversation.entries().size() == 2);
    CHECK(conversation.entries()[0].kind == EntryKind::User);
    CHECK(conversation.entries()[1].kind == EntryKind::Error);
    CHECK(conversation.entries()[1].text == "[modelo no encontrado] El modelo no existe.");
}

TEST_CASE("Conversation: error con texto parcial lo marca incompleto y fuera del historial",
          "[conversacion]") {
    Conversation conversation;
    REQUIRE(conversation.submit("Hola").has_value());
    conversation.append_delta("Respuesta a me");

    (void)conversation.finish_error(sample_error());
    CHECK_FALSE(conversation.busy());
    REQUIRE(conversation.entries().size() == 3);
    const Entry& partial = conversation.entries()[1];
    CHECK(partial.kind == EntryKind::Assistant);
    CHECK(partial.text == "Respuesta a me");
    CHECK(partial.incomplete);
    CHECK_FALSE(partial.in_progress);
    CHECK(conversation.entries()[2].kind == EntryKind::Error);
    REQUIRE(conversation.history().size() == 1);
    CHECK(conversation.history()[0].role == Role::System);
}

TEST_CASE("Conversation: finish_error devuelve el texto del usuario", "[conversacion]") {
    Conversation conversation;
    REQUIRE(conversation.submit("  ¿Qué hora es?  ").has_value());
    CHECK(conversation.finish_error(sample_error()) == "¿Qué hora es?");
}

TEST_CASE("Conversation: tras un error se puede reenviar", "[conversacion]") {
    Conversation conversation;
    REQUIRE(conversation.submit("Hola").has_value());
    const std::string restored = conversation.finish_error(sample_error());

    const std::optional<std::vector<chatbot::Message>> to_send = conversation.submit(restored);
    REQUIRE(to_send.has_value());
    // Sin dos mensajes de usuario seguidos.
    REQUIRE(to_send->size() == 2);
    CHECK((*to_send)[0].role == Role::System);
    CHECK((*to_send)[1].role == Role::User);
}

TEST_CASE("Conversation: orden y roles del historial tras varias vueltas", "[conversacion]") {
    Conversation conversation;

    REQUIRE(conversation.submit("Uno").has_value());
    conversation.append_delta("Respuesta uno");
    CHECK_FALSE(conversation.finish_success().has_value());

    REQUIRE(conversation.submit("Dos").has_value());
    conversation.append_delta("parcial");
    (void)conversation.finish_error(sample_error());

    REQUIRE(conversation.submit("Tres").has_value());
    (void)conversation.finish_success(); // Sin texto: error.

    const std::optional<std::vector<chatbot::Message>> to_send = conversation.submit("Cuatro");
    REQUIRE(to_send.has_value());
    conversation.append_delta("Respuesta cuatro");
    CHECK_FALSE(conversation.finish_success().has_value());

    const std::vector<chatbot::Message>& history = conversation.history();
    REQUIRE(history.size() == 5);
    CHECK(history[0].role == Role::System);
    CHECK(history[1].role == Role::User);
    CHECK(history[1].content == "Uno");
    CHECK(history[2].role == Role::Assistant);
    CHECK(history[2].content == "Respuesta uno");
    CHECK(history[3].role == Role::User);
    CHECK(history[3].content == "Cuatro");
    CHECK(history[4].role == Role::Assistant);
    CHECK(history[4].content == "Respuesta cuatro");
    check_no_empty_messages(conversation);

    // La copia enviada en la cuarta vuelta no incluye los intentos fallidos.
    REQUIRE(to_send->size() == 4);
    CHECK((*to_send)[3].content == "Cuatro");

    // En pantalla quedan todas las entradas, incluidos errores e incompletas.
    CHECK(conversation.entries().size() == 9);
}
