#include "conversation_list.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

namespace {

using chatbot::cli::ConversationList;
using chatbot::cli::ConversationSummary;
using chatbot::cli::ListAction;
using chatbot::cli::ListKey;

ConversationSummary item(const std::string& id, const std::string& title, bool readable = true) {
    ConversationSummary summary;
    summary.id = id;
    summary.title = title;
    summary.updated_at = "2026-10-02T23:58:00-06:00";
    summary.message_count = 4;
    summary.last_model = "modelo/x";
    summary.readable = readable;
    if (!readable) {
        summary.error = "versión 99 desconocida";
    }
    return summary;
}

std::vector<ConversationSummary> five() {
    return {item("a", "Uno"), item("b", "Dos"), item("c", "Tres", false), item("d", "Cuatro"),
            item("e", "Cinco")};
}

} // namespace

TEST_CASE("ConversationList: al abrir selecciona la conversación actual", "[lista]") {
    ConversationList list;
    list.open(five(), "d");
    CHECK(list.selected() == 3);
    CHECK(list.is_current(3));
    CHECK_FALSE(list.is_current(0));

    list.open(five(), "no-esta");
    CHECK(list.selected() == 0);
}

TEST_CASE("ConversationList: flechas, PgUp/PgDn y Home/End navegan con límites", "[lista]") {
    ConversationList list;
    list.open(five(), "");
    CHECK(list.handle(ListKey::Up, {}, 2).type == ListAction::Type::None);
    CHECK(list.selected() == 0);
    (void)list.handle(ListKey::Down, {}, 2);
    CHECK(list.selected() == 1);
    (void)list.handle(ListKey::PageDown, {}, 2);
    CHECK(list.selected() == 3);
    (void)list.handle(ListKey::PageDown, {}, 2);
    CHECK(list.selected() == 4);
    (void)list.handle(ListKey::Down, {}, 2);
    CHECK(list.selected() == 4);
    (void)list.handle(ListKey::PageUp, {}, 3);
    CHECK(list.selected() == 1);
    (void)list.handle(ListKey::End, {}, 2);
    CHECK(list.selected() == 4);
    (void)list.handle(ListKey::Home, {}, 2);
    CHECK(list.selected() == 0);
}

TEST_CASE("ConversationList: Enter abre solo las legibles y Esc cierra", "[lista]") {
    ConversationList list;
    list.open(five(), "");
    (void)list.handle(ListKey::Down, {}, 1);
    ListAction action = list.handle(ListKey::Enter, {}, 1);
    CHECK(action.type == ListAction::Type::Open);
    CHECK(action.id == "b");

    (void)list.handle(ListKey::Down, {}, 1); // "c" es ilegible.
    CHECK_FALSE(list.can_open(list.selected()));
    CHECK(list.handle(ListKey::Enter, {}, 1).type == ListAction::Type::None);
    CHECK(list.handle(ListKey::Escape, {}, 1).type == ListAction::Type::Close);
}

TEST_CASE("ConversationList: Supr pide confirmación y solo 's' borra", "[lista]") {
    ConversationList list;
    list.open(five(), "");
    CHECK_FALSE(list.confirmation().has_value());

    CHECK(list.handle(ListKey::Delete, {}, 1).type == ListAction::Type::None);
    REQUIRE(list.confirmation().has_value());
    CHECK(*list.confirmation() == "¿Borrar «Uno»? (s/n)");
    (void)list.handle(ListKey::Other, "x", 1); // Cancela.

    // Cualquier otra tecla cancela (incluidas 'S', 'n', Esc y las flechas).
    for (const auto& [key, character] :
         std::vector<std::pair<ListKey, std::string>>{{ListKey::Other, "S"},
                                                      {ListKey::Other, "n"},
                                                      {ListKey::Escape, ""},
                                                      {ListKey::Down, ""},
                                                      {ListKey::Enter, ""}}) {
        (void)list.handle(ListKey::Delete, {}, 1);
        REQUIRE(list.confirmation().has_value());
        CHECK(list.handle(key, character, 1).type == ListAction::Type::None);
        CHECK_FALSE(list.confirmation().has_value());
        CHECK(list.selected() == 0); // La tecla que cancela no hace nada más.
    }

    (void)list.handle(ListKey::Delete, {}, 1);
    const ListAction action = list.handle(ListKey::Other, "s", 1);
    CHECK(action.type == ListAction::Type::Delete);
    CHECK(action.id == "a");
    CHECK_FALSE(list.confirmation().has_value());
}

TEST_CASE("ConversationList: no se puede borrar una ilegible", "[lista]") {
    ConversationList list;
    list.open(five(), "");
    (void)list.handle(ListKey::Down, {}, 1);
    (void)list.handle(ListKey::Down, {}, 1);
    REQUIRE(list.selected() == 2);
    CHECK(list.handle(ListKey::Delete, {}, 1).type == ListAction::Type::None);
    CHECK_FALSE(list.confirmation().has_value());
}

TEST_CASE("ConversationList: tras borrar, la selección queda en el mismo lugar", "[lista]") {
    ConversationList list;
    list.open(five(), "");
    (void)list.handle(ListKey::End, {}, 1);
    std::vector<ConversationSummary> after = five();
    after.pop_back(); // Se borró "e" (la última).
    list.refresh(after);
    CHECK(list.selected() == 3);

    (void)list.handle(ListKey::Home, {}, 1);
    (void)list.handle(ListKey::Down, {}, 1); // "b"
    std::vector<ConversationSummary> without_b = {item("a", "Uno"), item("c", "Tres", false),
                                                  item("d", "Cuatro")};
    list.refresh(without_b);
    CHECK(list.selected() == 1);
}

TEST_CASE("ConversationList: vacía", "[lista]") {
    ConversationList list;
    list.open({}, "");
    CHECK(list.empty());
    CHECK(list.handle(ListKey::Down, {}, 5).type == ListAction::Type::None);
    CHECK(list.handle(ListKey::Enter, {}, 5).type == ListAction::Type::None);
    CHECK(list.handle(ListKey::Delete, {}, 5).type == ListAction::Type::None);
    CHECK(list.handle(ListKey::Escape, {}, 5).type == ListAction::Type::Close);
    CHECK(std::string{ConversationList::kEmptyMessage} == "No hay conversaciones guardadas.");
}

TEST_CASE("ConversationList: detalles de cada fila", "[lista]") {
    CHECK(ConversationList::format_date("2026-10-02T23:58:00-06:00") == "2026-10-02 23:58");
    CHECK(ConversationList::format_date("raro") == "raro");
    ConversationSummary one = item("a", "Uno");
    one.message_count = 1;
    CHECK(ConversationList::details(one) == "2026-10-02 23:58 · 1 mensaje · modelo/x");
    CHECK(ConversationList::details(item("b", "Dos")) == "2026-10-02 23:58 · 4 mensajes · modelo/x");
    CHECK(ConversationList::details(item("c", "Tres", false)) ==
          "ilegible: versión 99 desconocida");
}
