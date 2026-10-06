#include "conversation.h"
#include "system_prompt.h"

#include "temp_dir.hpp"

#include "chatbot/error.h"
#include "chatbot/types.h"

#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using chatbot::Role;
using chatbot::cli::Conversation;
using chatbot::cli::Entry;
using chatbot::cli::EntryKind;
using chatbot::cli::kDefaultSystemPrompt;

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
    const Conversation conversation{kDefaultSystemPrompt};
    REQUIRE(conversation.history().size() == 1);
    CHECK(conversation.history()[0].role == Role::System);
    CHECK(conversation.history()[0].content == kDefaultSystemPrompt);
    CHECK(conversation.entries().empty());
    CHECK_FALSE(conversation.busy());
}

TEST_CASE("Conversation: submit vacío o de solo espacios no hace nada", "[conversacion]") {
    Conversation conversation{kDefaultSystemPrompt};
    CHECK_FALSE(conversation.submit("").has_value());
    CHECK_FALSE(conversation.submit("   \t  ").has_value());
    CHECK(conversation.entries().empty());
    CHECK(conversation.history().size() == 1);
    CHECK_FALSE(conversation.busy());
}

TEST_CASE("Conversation: submit recorta espacios y devuelve una copia del historial",
          "[conversacion]") {
    Conversation conversation{kDefaultSystemPrompt};
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
    Conversation conversation{kDefaultSystemPrompt};
    REQUIRE(conversation.submit("Primero").has_value());
    CHECK_FALSE(conversation.submit("Segundo").has_value());
    CHECK(conversation.entries().size() == 1);
    CHECK(conversation.history().size() == 2);
}

TEST_CASE("Conversation: flujo exitoso con varios deltas", "[conversacion]") {
    Conversation conversation{kDefaultSystemPrompt};
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
    Conversation conversation{kDefaultSystemPrompt};
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
    Conversation conversation{kDefaultSystemPrompt};
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
    Conversation conversation{kDefaultSystemPrompt};
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
    Conversation conversation{kDefaultSystemPrompt};
    REQUIRE(conversation.submit("  ¿Qué hora es?  ").has_value());
    CHECK(conversation.finish_error(sample_error()) == "¿Qué hora es?");
}

TEST_CASE("Conversation: tras un error se puede reenviar", "[conversacion]") {
    Conversation conversation{kDefaultSystemPrompt};
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
    Conversation conversation{kDefaultSystemPrompt};

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

namespace {
chatbot::ChatError cancelled_error() {
    return chatbot::ChatError{chatbot::ErrorKind::Cancelled, 0,
                              "Petición cancelada por el usuario.", std::nullopt};
}
} // namespace

TEST_CASE("Conversation: cancelada con texto parcial la marca (cancelada)",
          "[conversacion][cancelacion]") {
    Conversation conversation{kDefaultSystemPrompt};
    REQUIRE(conversation.submit("Hola").has_value());
    conversation.append_delta("Respuesta a me");

    CHECK(conversation.finish_error(cancelled_error()) == "Hola");
    CHECK_FALSE(conversation.busy());
    REQUIRE(conversation.entries().size() == 2); // Sin entrada de error.
    const Entry& partial = conversation.entries()[1];
    CHECK(partial.kind == EntryKind::Assistant);
    CHECK(partial.cancelled);
    CHECK_FALSE(partial.incomplete);
    CHECK_FALSE(partial.in_progress);
    REQUIRE(conversation.history().size() == 1); // Ni el usuario ni el parcial.
    CHECK(conversation.history()[0].role == Role::System);
}

TEST_CASE("Conversation: cancelada sin texto agrega un aviso", "[conversacion][cancelacion]") {
    Conversation conversation{kDefaultSystemPrompt};
    REQUIRE(conversation.submit("Hola").has_value());

    CHECK(conversation.finish_error(cancelled_error()) == "Hola");
    CHECK_FALSE(conversation.busy());
    REQUIRE(conversation.entries().size() == 2);
    CHECK(conversation.entries()[1].kind == EntryKind::Notice);
    CHECK(conversation.entries()[1].text == "Respuesta cancelada.");
    CHECK(conversation.history().size() == 1);
    for (const Entry& entry : conversation.entries()) {
        CHECK(entry.kind != EntryKind::Error);
    }
}

namespace {
/// Una vuelta con texto que termina con el finish_reason dado.
Conversation finished_with(std::string_view finish_reason) {
    Conversation conversation{kDefaultSystemPrompt};
    REQUIRE(conversation.submit("Hola").has_value());
    conversation.append_delta("Texto real");
    CHECK_FALSE(conversation.finish_success(finish_reason).has_value());
    return conversation;
}
} // namespace

TEST_CASE("Conversation: finish_reason stop o vacío no agrega nota", "[conversacion][finish]") {
    for (const std::string_view reason : {std::string_view{"stop"}, std::string_view{""}}) {
        const Conversation conversation = finished_with(reason);
        REQUIRE(conversation.entries().size() == 2);
        CHECK(conversation.entries()[1].note.empty());
        REQUIRE(conversation.history().size() == 3);
        CHECK(conversation.history()[2].content == "Texto real");
    }
}

TEST_CASE("Conversation: finish_reason length entra al historial con nota",
          "[conversacion][finish]") {
    const Conversation conversation = finished_with("length");
    CHECK(conversation.entries()[1].note == "(cortada por límite de tokens)");
    CHECK_FALSE(conversation.entries()[1].incomplete);
    REQUIRE(conversation.history().size() == 3);
    CHECK(conversation.history()[2].role == Role::Assistant);
    CHECK(conversation.history()[2].content == "Texto real");
}

TEST_CASE("Conversation: finish_reason content_filter", "[conversacion][finish]") {
    const Conversation conversation = finished_with("content_filter");
    CHECK(conversation.entries()[1].note == "(detenida por el filtro de contenido)");
    CHECK(conversation.history().size() == 3);
}

TEST_CASE("Conversation: otro finish_reason muestra el valor", "[conversacion][finish]") {
    const Conversation conversation = finished_with("tool_calls");
    CHECK(conversation.entries()[1].note == "(terminó por: tool_calls)");
    CHECK(conversation.history().size() == 3);
}

TEST_CASE("Conversation: finish_reason sin texto sigue siendo error", "[conversacion][finish]") {
    Conversation conversation{kDefaultSystemPrompt};
    REQUIRE(conversation.submit("Hola").has_value());
    const std::optional<std::string> restored = conversation.finish_success("length");
    REQUIRE(restored.has_value());
    CHECK(*restored == "Hola");
    REQUIRE(conversation.entries().size() == 2);
    CHECK(conversation.entries()[1].kind == EntryKind::Error);
    CHECK(conversation.entries()[1].text == "[respuesta inválida] El modelo no devolvió texto.");
    CHECK(conversation.history().size() == 1);
}

TEST_CASE("Conversation: ida y vuelta a la forma guardable", "[conversacion][persistencia]") {
    Conversation original{kDefaultSystemPrompt};
    REQUIRE(original.submit("  Primera\n  pregunta  ").has_value());
    original.append_delta("Respuesta **uno**");
    REQUIRE_FALSE(original.finish_success("stop", "modelo/a").has_value());
    REQUIRE(original.submit("Segunda").has_value());
    original.append_delta("Respuesta cortada");
    REQUIRE_FALSE(original.finish_success("length", "modelo/b").has_value());
    original.set_identity("20261002-235800-a1b2c3", "2026-10-02T23:58:00-06:00");

    const chatbot::cli::StoredConversation stored = original.to_stored("2026-10-02T23:59:12-06:00");
    CHECK(stored.id == "20261002-235800-a1b2c3");
    CHECK(stored.created_at == "2026-10-02T23:58:00-06:00");
    CHECK(stored.updated_at == "2026-10-02T23:59:12-06:00");
    CHECK(stored.title == "Primera pregunta"); // El salto de línea se colapsa.
    REQUIRE(stored.messages.size() == 4);
    CHECK(stored.messages[0].role == Role::User);
    CHECK(stored.messages[1].model == "modelo/a");
    CHECK(stored.messages[1].finish_reason == "stop");
    CHECK(stored.messages[3].model == "modelo/b");
    CHECK(stored.messages[3].finish_reason == "length");

    const Conversation rebuilt = Conversation::from_stored(stored, kDefaultSystemPrompt);
    CHECK(rebuilt.id() == original.id());
    CHECK(rebuilt.title() == original.title());
    CHECK(rebuilt.created_at() == original.created_at());
    CHECK(rebuilt.last_model() == "modelo/b");
    CHECK_FALSE(rebuilt.busy());
    REQUIRE(rebuilt.history().size() == original.history().size());
    for (std::size_t i = 0; i < rebuilt.history().size(); ++i) {
        CHECK(rebuilt.history()[i].role == original.history()[i].role);
        CHECK(rebuilt.history()[i].content == original.history()[i].content);
    }
    REQUIRE(rebuilt.entries().size() == original.entries().size());
    for (std::size_t i = 0; i < rebuilt.entries().size(); ++i) {
        CHECK(rebuilt.entries()[i].kind == original.entries()[i].kind);
        CHECK(rebuilt.entries()[i].text == original.entries()[i].text);
        CHECK(rebuilt.entries()[i].note == original.entries()[i].note);
    }
    // La nota de finish_reason reaparece.
    CHECK(rebuilt.entries()[3].note == "(cortada por límite de tokens)");
}

TEST_CASE("Conversation: al reconstruir se usa el prompt dado",
          "[conversacion][persistencia]") {
    chatbot::cli::StoredConversation stored;
    stored.id = "20261002-235800-a1b2c3";
    stored.title = "Hola";
    stored.messages = {{Role::User, "Hola", "", ""}, {Role::Assistant, "Qué tal", "m", "stop"}};
    const Conversation rebuilt = Conversation::from_stored(stored, kDefaultSystemPrompt);
    REQUIRE(rebuilt.history().size() == 3);
    CHECK(rebuilt.history()[0].role == Role::System);
    CHECK(rebuilt.history()[0].content == kDefaultSystemPrompt);
}

TEST_CASE("Conversation: errores y cancelaciones no entran a lo guardable",
          "[conversacion][persistencia]") {
    Conversation conversation{kDefaultSystemPrompt};
    REQUIRE(conversation.submit("Uno").has_value());
    conversation.append_delta("Bien");
    REQUIRE_FALSE(conversation.finish_success("stop", "m").has_value());
    REQUIRE(conversation.submit("Dos").has_value());
    conversation.append_delta("parcial");
    (void)conversation.finish_error(sample_error());
    REQUIRE(conversation.submit("Tres").has_value());
    conversation.append_delta("parcial");
    (void)conversation.finish_error(cancelled_error());
    REQUIRE(conversation.submit("Cuatro").has_value());
    (void)conversation.finish_success("stop", "m"); // Sin texto: error.
    conversation.add_error("No se pudo guardar la conversación: prueba");
    conversation.add_notice("Un aviso");

    const chatbot::cli::StoredConversation stored = conversation.to_stored("x");
    REQUIRE(stored.messages.size() == 2);
    CHECK(stored.messages[0].content == "Uno");
    CHECK(stored.messages[1].content == "Bien");
    CHECK(conversation.has_turns());
}

TEST_CASE("Conversation: sin pares terminados no hay nada que guardar ni título",
          "[conversacion][persistencia]") {
    Conversation conversation{kDefaultSystemPrompt};
    CHECK_FALSE(conversation.has_turns());
    CHECK(conversation.title().empty());
    REQUIRE(conversation.submit("Hola").has_value());
    (void)conversation.finish_error(cancelled_error());
    CHECK_FALSE(conversation.has_turns());
    CHECK(conversation.title().empty());
    CHECK(conversation.id().empty());
}

TEST_CASE("Conversation: add_error y add_notice agregan entradas", "[conversacion][persistencia]") {
    Conversation conversation{kDefaultSystemPrompt};
    conversation.add_error("No se pudo guardar la conversación: disco lleno");
    conversation.add_notice("Esta conversación usó a; se continúa con b.");
    REQUIRE(conversation.entries().size() == 2);
    CHECK(conversation.entries()[0].kind == EntryKind::Error);
    CHECK(conversation.entries()[0].text == "No se pudo guardar la conversación: disco lleno");
    CHECK(conversation.entries()[1].kind == EntryKind::Notice);
}

TEST_CASE("save_conversation: sin pares no crea archivo; con un par lo crea",
          "[conversacion][persistencia]") {
    const chatbot_test::ScopedTempDir temp;
    const chatbot::cli::ConversationStore store{temp.string()};
    Conversation conversation{kDefaultSystemPrompt};

    chatbot::cli::save_conversation(conversation, store);
    CHECK(store.list().empty());
    CHECK(conversation.id().empty());

    REQUIRE(conversation.submit("Hola").has_value());
    conversation.append_delta("Qué tal");
    REQUIRE_FALSE(conversation.finish_success("stop", "m").has_value());
    chatbot::cli::save_conversation(conversation, store);

    REQUIRE_FALSE(conversation.id().empty());
    CHECK_FALSE(conversation.created_at().empty());
    const auto list = store.list();
    REQUIRE(list.size() == 1);
    CHECK(list[0].id == conversation.id());
    CHECK(list[0].title == "Hola");
    CHECK(list[0].message_count == 2);

    // El segundo guardado usa el mismo id (no crea otro archivo).
    REQUIRE(conversation.submit("Otra").has_value());
    conversation.append_delta("Respuesta");
    REQUIRE_FALSE(conversation.finish_success("stop", "m").has_value());
    const std::string first_id = conversation.id();
    chatbot::cli::save_conversation(conversation, store);
    CHECK(conversation.id() == first_id);
    REQUIRE(store.list().size() == 1);
    CHECK(store.list()[0].message_count == 4);
}

TEST_CASE("save_conversation: si falla agrega un error y el chat sigue",
          "[conversacion][persistencia]") {
    const chatbot_test::ScopedTempDir temp;
    const auto not_a_dir = temp.path() / "archivo";
    { std::ofstream{not_a_dir} << "x"; }
    const chatbot::cli::ConversationStore store{not_a_dir.string()};
    Conversation conversation{kDefaultSystemPrompt};
    REQUIRE(conversation.submit("Hola").has_value());
    conversation.append_delta("Qué tal");
    REQUIRE_FALSE(conversation.finish_success("stop", "m").has_value());

    chatbot::cli::save_conversation(conversation, store);
    REQUIRE(conversation.entries().size() == 3);
    CHECK(conversation.entries()[2].kind == EntryKind::Error);
    CHECK(conversation.entries()[2].text.rfind("No se pudo guardar la conversación: ", 0) == 0);
    // Se puede seguir conversando.
    CHECK(conversation.submit("Sigo").has_value());
}

namespace {
/// Conversación con un par terminado y las instrucciones dadas.
Conversation with_one_turn(std::string_view system_prompt) {
    Conversation conversation{system_prompt};
    REQUIRE(conversation.submit("Hola").has_value());
    conversation.append_delta("Qué tal");
    REQUIRE_FALSE(conversation.finish_success("stop", "m").has_value());
    return conversation;
}

/// Mismos mensajes guardables (el mensaje de sistema nunca está ahí).
void check_same_stored(const Conversation& a, const Conversation& b) {
    const auto left = a.to_stored("t").messages;
    const auto right = b.to_stored("t").messages;
    REQUIRE(left.size() == right.size());
    for (std::size_t i = 0; i < left.size(); ++i) {
        CHECK(left[i].role == right[i].role);
        CHECK(left[i].content == right[i].content);
        CHECK(left[i].model == right[i].model);
        CHECK(left[i].finish_reason == right[i].finish_reason);
    }
}
} // namespace

TEST_CASE("Conversation: instrucciones de sistema personalizadas", "[conversacion][sistema]") {
    const std::string prompt = "Responde solo con la palabra PIÑA.\nNunca expliques.";
    Conversation conversation{prompt};
    REQUIRE(conversation.history().size() == 1);
    CHECK(conversation.history()[0].role == Role::System);
    CHECK(conversation.history()[0].content == prompt);
    const auto sent = conversation.submit("Hola");
    REQUIRE(sent.has_value());
    REQUIRE(sent->size() == 2);
    CHECK((*sent)[0].content == prompt);
}

TEST_CASE("Conversation: sin instrucciones el primer mensaje es del usuario",
          "[conversacion][sistema]") {
    Conversation conversation{""};
    CHECK(conversation.history().empty());
    const auto sent = conversation.submit("Hola");
    REQUIRE(sent.has_value());
    REQUIRE(sent->size() == 1);
    CHECK((*sent)[0].role == Role::User);
    check_no_empty_messages(conversation);
}

TEST_CASE("Conversation: from_stored usa las instrucciones dadas", "[conversacion][sistema]") {
    chatbot::cli::StoredConversation stored;
    stored.id = "20261002-235800-a1b2c3";
    stored.messages = {{Role::User, "Hola", "", ""}, {Role::Assistant, "Qué tal", "m", "stop"}};

    const Conversation custom = Conversation::from_stored(stored, "Sé breve.");
    REQUIRE(custom.history().size() == 3);
    CHECK(custom.history()[0].role == Role::System);
    CHECK(custom.history()[0].content == "Sé breve.");

    const Conversation none = Conversation::from_stored(stored, "");
    REQUIRE(none.history().size() == 2);
    CHECK(none.history()[0].role == Role::User);
    CHECK(none.history()[1].role == Role::Assistant);
}

TEST_CASE("Conversation: set_system_prompt reemplaza, quita y agrega",
          "[conversacion][sistema]") {
    Conversation conversation = with_one_turn(kDefaultSystemPrompt);
    const Conversation before = with_one_turn(kDefaultSystemPrompt);
    const std::size_t entries = conversation.entries().size();

    SECTION("reemplazar") {
        REQUIRE(conversation.set_system_prompt("Nuevo"));
        REQUIRE(conversation.history().size() == 3);
        CHECK(conversation.history()[0].role == Role::System);
        CHECK(conversation.history()[0].content == "Nuevo");
    }
    SECTION("quitar") {
        REQUIRE(conversation.set_system_prompt(""));
        REQUIRE(conversation.history().size() == 2);
        CHECK(conversation.history()[0].role == Role::User);
        // Quitarlo dos veces no quita el mensaje del usuario.
        REQUIRE(conversation.set_system_prompt(""));
        CHECK(conversation.history().size() == 2);
    }
    SECTION("agregar") {
        REQUIRE(conversation.set_system_prompt(""));
        REQUIRE(conversation.set_system_prompt("Otra vez"));
        REQUIRE(conversation.history().size() == 3);
        CHECK(conversation.history()[0].role == Role::System);
        CHECK(conversation.history()[0].content == "Otra vez");
        CHECK(conversation.history()[1].content == "Hola");
    }
    CHECK(conversation.entries().size() == entries);
    CHECK(conversation.history().back().content == "Qué tal");
    check_same_stored(conversation, before);
}

TEST_CASE("Conversation: el envío siguiente lleva las instrucciones nuevas",
          "[conversacion][sistema]") {
    Conversation conversation = with_one_turn("Viejo");
    REQUIRE(conversation.set_system_prompt("Responde solo con la palabra PIÑA"));
    const auto sent = conversation.submit("Otra");
    REQUIRE(sent.has_value());
    REQUIRE(sent->size() == 4);
    CHECK(sent->front().role == Role::System);
    CHECK(sent->front().content == "Responde solo con la palabra PIÑA");
    CHECK(sent->back().content == "Otra");
}

TEST_CASE("Conversation: set_system_prompt no hace nada con una respuesta en curso",
          "[conversacion][sistema]") {
    Conversation conversation{"Original"};
    REQUIRE(conversation.submit("Hola").has_value());
    conversation.append_delta("Parcial");
    REQUIRE(conversation.busy());
    CHECK_FALSE(conversation.set_system_prompt("Nuevo"));
    CHECK_FALSE(conversation.set_system_prompt(""));
    REQUIRE(conversation.history().size() == 2);
    CHECK(conversation.history()[0].content == "Original");
    CHECK(conversation.history()[1].content == "Hola");
    CHECK(conversation.entries().size() == 2);
    REQUIRE_FALSE(conversation.finish_success("stop").has_value());
    CHECK(conversation.history()[0].content == "Original");
}
