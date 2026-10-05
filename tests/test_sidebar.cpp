#include "sidebar.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

#include <string>
#include <vector>

namespace {

using chatbot::cli::CalendarDay;
using chatbot::cli::ConversationSummary;
using chatbot::cli::ListAction;
using chatbot::cli::ListKey;
using chatbot::cli::Sidebar;
using chatbot::cli::SidebarRow;

constexpr CalendarDay kToday{2026, 10, 4};

ConversationSummary item(std::string id, std::string updated_at, bool readable = true) {
    ConversationSummary summary;
    summary.title = "Título " + id;
    summary.id = std::move(id);
    summary.updated_at = std::move(updated_at);
    summary.readable = readable;
    return summary;
}

/// Hoy, ayer, hace 3 días, hace 10 días, el año pasado y una ilegible, en el
/// orden en que las da el almacén (la más reciente primero; ilegibles al final).
std::vector<ConversationSummary> sample() {
    return {
        item("hoy", "2026-10-04T10:00:00-06:00"),
        item("ayer", "2026-10-03T12:00:00-06:00"),
        item("hace3", "2026-10-01T09:30:00-06:00"),
        item("hace10", "2026-09-24T18:00:00-06:00"),
        item("antiguo", "2025-12-15T08:00:00-06:00"),
        item("roto.json", "", false),
    };
}

/// Las filas como texto: "+", "⚙", "#Grupo" o el id de la conversación.
std::vector<std::string> describe(const Sidebar& sidebar, const std::vector<SidebarRow>& rows) {
    std::vector<std::string> out;
    for (const SidebarRow& row : rows) {
        switch (row.kind) {
        case SidebarRow::Kind::New:
            out.emplace_back("+");
            break;
        case SidebarRow::Kind::Settings:
            out.emplace_back("⚙");
            break;
        case SidebarRow::Kind::Header:
            out.push_back("#" + row.text);
            break;
        case SidebarRow::Kind::Conversation:
            out.push_back(sidebar.list().items()[row.index].id);
            break;
        }
    }
    return out;
}

/// Id de la conversación seleccionada, o "+" / "⚙" si es una fila fija.
std::string selected(const Sidebar& sidebar) {
    if (sidebar.new_selected()) {
        return "+";
    }
    if (sidebar.settings_selected()) {
        return "⚙";
    }
    return sidebar.list().items()[sidebar.list().selected()].id;
}

} // namespace

TEST_CASE("barra: grupos por fecha con hoy fijo", "[barra]") {
    Sidebar sidebar;
    sidebar.open(sample(), "");
    CHECK(describe(sidebar, sidebar.rows(kToday)) ==
          std::vector<std::string>{"+", "⚙", "#Hoy", "hoy", "#Ayer", "ayer", "#Últimos 7 días", "hace3",
                                   "#24 sep", "hace10", "#15 dic 2025", "antiguo", "#Sin fecha",
                                   "roto.json"});
}

TEST_CASE("barra: los grupos vacíos no aparecen", "[barra]") {
    Sidebar sidebar;
    sidebar.open({item("a", "2026-10-04T08:00:00-06:00"), item("b", "2026-10-04T07:00:00-06:00"),
                  item("c", "2026-09-02T07:00:00-06:00")},
                 "");
    CHECK(describe(sidebar, sidebar.rows(kToday)) ==
          std::vector<std::string>{"+", "⚙", "#Hoy", "a", "b", "#2 sep", "c"});
    Sidebar empty;
    empty.open({}, "");
    CHECK(describe(empty, empty.rows(kToday)) == std::vector<std::string>{"+", "⚙"});
}

TEST_CASE("barra: nombre del grupo según los días", "[barra]") {
    using chatbot::cli::group_label;
    CHECK(group_label({2026, 10, 4}, kToday) == "Hoy");
    CHECK(group_label({2026, 10, 5}, kToday) == "Hoy"); // Reloj adelantado.
    CHECK(group_label({2026, 10, 3}, kToday) == "Ayer");
    CHECK(group_label({2026, 10, 2}, kToday) == "Últimos 7 días");
    CHECK(group_label({2026, 9, 28}, kToday) == "Últimos 7 días"); // Hace 6 días.
    CHECK(group_label({2026, 9, 27}, kToday) == "27 sep");         // Hace 7 días.
    CHECK(group_label({2026, 1, 1}, kToday) == "1 ene");
    CHECK(group_label({2025, 12, 31}, {2026, 1, 1}) == "Ayer"); // Cambio de año.
    CHECK(group_label({2025, 12, 15}, kToday) == "15 dic 2025");
}

TEST_CASE("barra: cambio de día a medianoche", "[barra]") {
    Sidebar sidebar;
    sidebar.open({item("despues", "2026-10-04T00:01:00-06:00"),
                  item("antes", "2026-10-03T23:59:00-06:00")},
                 "");
    CHECK(describe(sidebar, sidebar.rows(kToday)) ==
          std::vector<std::string>{"+", "⚙", "#Hoy", "despues", "#Ayer", "antes"});
}

TEST_CASE("barra: fechas de updated_at", "[barra]") {
    using chatbot::cli::day_of;
    CHECK(day_of("2026-10-02T23:58:00-06:00") == CalendarDay{2026, 10, 2});
    CHECK_FALSE(day_of("").has_value());
    CHECK_FALSE(day_of("2026-02-30T10:00:00-06:00").has_value());
    CHECK_FALSE(day_of("ayer").has_value());
}

TEST_CASE("barra: la navegación salta encabezados", "[barra]") {
    Sidebar sidebar;
    sidebar.open(sample(), "");
    CHECK(selected(sidebar) == "+");
    CHECK(sidebar.handle(ListKey::Up, {}, 5).type == ListAction::Type::None);
    CHECK(selected(sidebar) == "+");
    (void)sidebar.handle(ListKey::Down, {}, 5);
    CHECK(selected(sidebar) == "⚙");
    CHECK(sidebar.selected_row(sidebar.rows(kToday)) == 1);
    (void)sidebar.handle(ListKey::Down, {}, 5); // Salta "Hoy".
    CHECK(selected(sidebar) == "hoy");
    (void)sidebar.handle(ListKey::Down, {}, 5); // Salta "Ayer".
    CHECK(selected(sidebar) == "ayer");
    CHECK(sidebar.selected_row(sidebar.rows(kToday)) == 5);
    (void)sidebar.handle(ListKey::Up, {}, 5);
    (void)sidebar.handle(ListKey::Up, {}, 5);
    CHECK(selected(sidebar) == "⚙");
    (void)sidebar.handle(ListKey::Up, {}, 5);
    CHECK(selected(sidebar) == "+");
    (void)sidebar.handle(ListKey::End, {}, 5);
    CHECK(selected(sidebar) == "roto.json");
    (void)sidebar.handle(ListKey::Home, {}, 5);
    CHECK(selected(sidebar) == "hoy");
    (void)sidebar.handle(ListKey::PageDown, {}, 3);
    CHECK(selected(sidebar) == "hace10");
    (void)sidebar.handle(ListKey::PageUp, {}, 3);
    CHECK(selected(sidebar) == "hoy");
    (void)sidebar.handle(ListKey::PageUp, {}, 3);
    CHECK(selected(sidebar) == "⚙");
    (void)sidebar.handle(ListKey::PageUp, {}, 3);
    CHECK(selected(sidebar) == "+");
    // PgDn desde una fila fija: la conversación de una página (índice 2).
    (void)sidebar.handle(ListKey::PageDown, {}, 3);
    CHECK(selected(sidebar) == "hace3");
}

TEST_CASE("barra: sin conversaciones solo se mueve entre las filas fijas", "[barra]") {
    Sidebar sidebar;
    sidebar.open({}, "");
    CHECK(sidebar.handle(ListKey::Home, {}, 5).type == ListAction::Type::None);
    CHECK(sidebar.new_selected());
    for (const ListKey key : {ListKey::Down, ListKey::End, ListKey::PageDown, ListKey::Down}) {
        CHECK(sidebar.handle(key, {}, 5).type == ListAction::Type::None);
        CHECK(sidebar.settings_selected());
    }
    CHECK(sidebar.handle(ListKey::Delete, {}, 5).type == ListAction::Type::None);
    CHECK_FALSE(sidebar.confirmation().has_value());
    CHECK(sidebar.handle(ListKey::Enter, {}, 5).type == ListAction::Type::Settings);
    (void)sidebar.handle(ListKey::Up, {}, 5);
    CHECK(sidebar.new_selected());
    CHECK(sidebar.handle(ListKey::Enter, {}, 5).type == ListAction::Type::New);
}

TEST_CASE("barra: Enter, Supr y Esc", "[barra]") {
    Sidebar sidebar;
    sidebar.open(sample(), "");
    CHECK(sidebar.handle(ListKey::Enter, {}, 5).type == ListAction::Type::New);
    CHECK(sidebar.handle(ListKey::Delete, {}, 5).type == ListAction::Type::None); // En "+ Nueva".
    CHECK_FALSE(sidebar.confirmation().has_value());

    (void)sidebar.handle(ListKey::Down, {}, 5);
    CHECK(sidebar.handle(ListKey::Enter, {}, 5).type == ListAction::Type::Settings);
    CHECK(sidebar.handle(ListKey::Delete, {}, 5).type == ListAction::Type::None); // En "⚙".
    CHECK_FALSE(sidebar.confirmation().has_value());
    (void)sidebar.handle(ListKey::Down, {}, 5);
    const ListAction open = sidebar.handle(ListKey::Enter, {}, 5);
    CHECK(open.type == ListAction::Type::Open);
    CHECK(open.id == "hoy");

    CHECK(sidebar.handle(ListKey::Delete, {}, 5).type == ListAction::Type::None);
    REQUIRE(sidebar.confirmation().has_value());
    CHECK(*sidebar.confirmation() == "¿Borrar «Título hoy»? (s/n)");
    const ListAction remove = sidebar.handle(ListKey::Other, "s", 5);
    CHECK(remove.type == ListAction::Type::Delete);
    CHECK(remove.id == "hoy");

    // Esc con confirmación la cancela; sin ella, devuelve el foco.
    (void)sidebar.handle(ListKey::Delete, {}, 5);
    CHECK(sidebar.handle(ListKey::Escape, {}, 5).type == ListAction::Type::None);
    CHECK_FALSE(sidebar.confirmation().has_value());
    CHECK(sidebar.handle(ListKey::Escape, {}, 5).type == ListAction::Type::Close);

    // Las ilegibles no se abren ni se borran.
    (void)sidebar.handle(ListKey::End, {}, 5);
    CHECK(sidebar.handle(ListKey::Enter, {}, 5).type == ListAction::Type::None);
    (void)sidebar.handle(ListKey::Delete, {}, 5);
    CHECK_FALSE(sidebar.confirmation().has_value());
}

TEST_CASE("barra: clic por línea, también desplazada", "[barra]") {
    Sidebar sidebar;
    sidebar.open(sample(), "");
    auto rows = sidebar.rows(kToday);
    sidebar.fit(rows, 5);
    REQUIRE(sidebar.top() == 0);
    CHECK(sidebar.click(0, rows).type == ListAction::Type::New);
    CHECK(sidebar.click(1, rows).type == ListAction::Type::Settings);
    CHECK(selected(sidebar) == "⚙");
    CHECK(sidebar.click(2, rows).type == ListAction::Type::None); // "Hoy".
    const ListAction first = sidebar.click(3, rows);
    CHECK(first.type == ListAction::Type::Open);
    CHECK(first.id == "hoy");
    CHECK(selected(sidebar) == "hoy");
    CHECK(sidebar.click(40, rows).type == ListAction::Type::None); // Debajo de la última.

    // Al final de la lista la vista baja: 14 filas, 5 visibles → desde la 9.
    (void)sidebar.handle(ListKey::End, {}, 5);
    sidebar.fit(rows, 5);
    CHECK(sidebar.top() == 9);
    const ListAction shifted = sidebar.click(0, rows); // Fila 9: "hace10".
    CHECK(shifted.type == ListAction::Type::Open);
    CHECK(shifted.id == "hace10");
    CHECK(sidebar.click(4, rows).type == ListAction::Type::None); // Ilegible: se selecciona, no abre.
    CHECK(selected(sidebar) == "roto.json");
}

TEST_CASE("barra: la rueda desplaza y la selección queda a la vista", "[barra]") {
    Sidebar sidebar;
    sidebar.open(sample(), "");
    const auto rows = sidebar.rows(kToday);
    sidebar.fit(rows, 5);
    sidebar.scroll(3, rows, 5);
    CHECK(sidebar.top() == 3);
    // "+ Nueva" (fila 0) salió de la vista: pasa a la primera visible.
    CHECK(selected(sidebar) == "hoy");
    sidebar.scroll(100, rows, 5);
    CHECK(sidebar.top() == 9); // No pasa del final.
    CHECK(selected(sidebar) == "hace10");
    sidebar.scroll(-100, rows, 5);
    CHECK(sidebar.top() == 0);
    // "hace10" (fila 9) salió por abajo: pasa a la última visible.
    CHECK(selected(sidebar) == "hoy");
}

TEST_CASE("barra: la selección se conserva al refrescar", "[barra]") {
    Sidebar sidebar;
    sidebar.open(sample(), "hace3");
    CHECK(selected(sidebar) == "hace3");
    CHECK(sidebar.list().is_current(2));

    // Se guardó "hace3": sube a "Hoy" y sigue seleccionada.
    std::vector<ConversationSummary> items = sample();
    items[2].updated_at = "2026-10-04T11:00:00-06:00";
    std::rotate(items.begin(), items.begin() + 2, items.begin() + 3);
    sidebar.refresh(items, "hace3");
    CHECK(selected(sidebar) == "hace3");
    CHECK(describe(sidebar, sidebar.rows(kToday)).at(3) == "hace3");

    // Se borró: queda la misma posición.
    items.erase(items.begin());
    sidebar.refresh(items, "");
    CHECK(selected(sidebar) == "hoy");
    CHECK_FALSE(sidebar.list().is_current(0));

    // Sin conversaciones: "+ Nueva".
    sidebar.refresh({}, "");
    CHECK(sidebar.new_selected());
}
