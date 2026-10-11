#include <catch2/catch_test_macros.hpp>

#include "command_menu.h"

#include <string>
#include <string_view>
#include <vector>

using chatbot::cli::CommandMenu;
using chatbot::cli::CommandMenuAction;
using chatbot::cli::kCommands;
using chatbot::cli::ListKey;

namespace {

/// Nombres de los comandos que se ven, en orden.
std::vector<std::string> shown(const CommandMenu& menu) {
    std::vector<std::string> names;
    for (const std::size_t index : menu.matches()) {
        names.emplace_back(kCommands[index].name);
    }
    return names;
}

/// Nombre del comando del cursor.
std::string at_cursor(const CommandMenu& menu) {
    REQUIRE(menu.cursor() < menu.matches().size());
    return std::string{kCommands[menu.matches()[menu.cursor()]].name};
}

const std::vector<std::string> kAll = {"/buscar", "/copiar", "/guardar", "/exportar"};

} // namespace

TEST_CASE("El menú se abre mientras se escribe el nombre de un comando", "[command_menu]") {
    CommandMenu menu;
    CHECK_FALSE(menu.is_open());

    menu.update("/", true);
    CHECK(menu.is_open());
    CHECK(shown(menu) == kAll);

    menu.update("/b", true);
    CHECK(menu.is_open());

    SECTION("Se cierra con un espacio o un salto de línea") {
        menu.update("/buscar ", true);
        CHECK_FALSE(menu.is_open());
        menu.update("/buscar\nhola", true);
        CHECK_FALSE(menu.is_open());
        menu.update("/b", true);
        CHECK(menu.is_open());
    }
    SECTION("Se cierra si el texto no empieza con /") {
        menu.update("", true);
        CHECK_FALSE(menu.is_open());
        menu.update("hola /buscar", true);
        CHECK_FALSE(menu.is_open());
        menu.update(" /buscar", true);
        CHECK_FALSE(menu.is_open());
    }
    SECTION("Solo con el foco en la caja") {
        menu.update("/b", false);
        CHECK_FALSE(menu.is_open());
        CHECK(menu.handle(ListKey::Down).type == CommandMenuAction::Type::Pass);
        menu.update("/b", true);
        CHECK(menu.is_open());
    }
}

TEST_CASE("El menú filtra por prefijo sin distinguir mayúsculas", "[command_menu]") {
    CommandMenu menu;
    menu.update("/g", true);
    CHECK(shown(menu) == std::vector<std::string>{"/guardar"});
    menu.update("/GU", true);
    CHECK(shown(menu) == std::vector<std::string>{"/guardar"});
    menu.update("/e", true);
    CHECK(shown(menu) == std::vector<std::string>{"/exportar"});
    menu.update("/exportar", true);
    CHECK(shown(menu) == std::vector<std::string>{"/exportar"});
    // Orden del registro.
    menu.update("/", true);
    CHECK(shown(menu) == kAll);
}

TEST_CASE("Sin coincidencias, el menú sigue abierto y deja pasar las teclas", "[command_menu]") {
    CommandMenu menu;
    menu.update("/xyz", true);
    CHECK(menu.is_open());
    CHECK(menu.matches().empty());
    CHECK(menu.handle(ListKey::Down).type == CommandMenuAction::Type::Pass);
    CHECK(menu.handle(ListKey::Up).type == CommandMenuAction::Type::Pass);
    CHECK(menu.handle(ListKey::Tab).type == CommandMenuAction::Type::Pass);
    // Enter: se envía como mensaje normal.
    CHECK(menu.handle(ListKey::Enter).type == CommandMenuAction::Type::Pass);
    CHECK(menu.choose(0).type == CommandMenuAction::Type::Pass);
    // /buscarx tampoco es un comando.
    menu.update("/buscarx", true);
    CHECK(menu.matches().empty());
    // Esc sí cierra.
    CHECK(menu.handle(ListKey::Escape).type == CommandMenuAction::Type::Handled);
    CHECK_FALSE(menu.is_open());
}

TEST_CASE("El cursor del menú da la vuelta en los extremos", "[command_menu]") {
    CommandMenu menu;
    menu.update("/", true);
    CHECK(at_cursor(menu) == "/buscar");
    CHECK(menu.handle(ListKey::Up).type == CommandMenuAction::Type::Handled);
    CHECK(at_cursor(menu) == "/exportar");
    CHECK(menu.handle(ListKey::Down).type == CommandMenuAction::Type::Handled);
    CHECK(at_cursor(menu) == "/buscar");
    (void)menu.handle(ListKey::Down);
    CHECK(at_cursor(menu) == "/copiar");

    SECTION("Home, End, PgUp y PgDn van a los extremos") {
        CHECK(menu.handle(ListKey::End).type == CommandMenuAction::Type::Handled);
        CHECK(at_cursor(menu) == "/exportar");
        (void)menu.handle(ListKey::Home);
        CHECK(at_cursor(menu) == "/buscar");
        (void)menu.handle(ListKey::PageDown);
        CHECK(at_cursor(menu) == "/exportar");
        (void)menu.handle(ListKey::PageUp);
        CHECK(at_cursor(menu) == "/buscar");
    }
    SECTION("Con una sola fila, el cursor no se mueve") {
        menu.update("/e", true);
        (void)menu.handle(ListKey::Down);
        CHECK(at_cursor(menu) == "/exportar");
        (void)menu.handle(ListKey::Up);
        CHECK(at_cursor(menu) == "/exportar");
    }
    SECTION("Otras teclas pasan") {
        CHECK(menu.handle(ListKey::Other).type == CommandMenuAction::Type::Pass);
        CHECK(menu.handle(ListKey::Delete).type == CommandMenuAction::Type::Pass);
        CHECK(at_cursor(menu) == "/copiar");
    }
}

TEST_CASE("El cursor queda en rango cuando cambia el filtro", "[command_menu]") {
    CommandMenu menu;
    menu.update("/", true);
    (void)menu.handle(ListKey::End); // /exportar, la cuarta fila.
    menu.update("/g", true);         // Una sola fila.
    CHECK(menu.cursor() == 0);
    CHECK(at_cursor(menu) == "/guardar");

    SECTION("Si el comando del cursor sigue, el cursor se queda en él") {
        menu.update("/", true);
        CHECK(at_cursor(menu) == "/guardar");
    }
    SECTION("Sin coincidencias y de vuelta") {
        menu.update("/gx", true);
        CHECK(menu.matches().empty());
        CHECK(menu.cursor() == 0);
        menu.update("/", true);
        CHECK(menu.cursor() < menu.matches().size());
    }
    SECTION("Al cerrarse y volver a abrirse, empieza arriba") {
        menu.update("", true);
        menu.update("/", true);
        CHECK(at_cursor(menu) == "/buscar");
    }
}

TEST_CASE("Tab completa el comando del cursor", "[command_menu]") {
    CommandMenu menu;
    SECTION("Con argumentos: el nombre y un espacio") {
        menu.update("/b", true);
        const CommandMenuAction action = menu.handle(ListKey::Tab);
        CHECK(action.type == CommandMenuAction::Type::Complete);
        CHECK(action.text == "/buscar ");
        CHECK_FALSE(menu.is_open());
        menu.update(action.text, true);
        CHECK_FALSE(menu.is_open());
    }
    SECTION("Con argumentos opcionales también lleva el espacio") {
        menu.update("/c", true);
        CHECK(menu.handle(ListKey::Tab).text == "/copiar ");
    }
    SECTION("Sin argumentos: solo el nombre, y el menú no se vuelve a abrir") {
        menu.update("/ex", true);
        const CommandMenuAction action = menu.handle(ListKey::Tab);
        CHECK(action.type == CommandMenuAction::Type::Complete);
        CHECK(action.text == "/exportar");
        menu.update(action.text, true);
        CHECK_FALSE(menu.is_open());
        // Hasta que cambie el texto.
        menu.update("/exporta", true);
        CHECK(menu.is_open());
    }
    SECTION("Lo que se inserta va en minúsculas") {
        menu.update("/BUS", true);
        CHECK(menu.handle(ListKey::Tab).text == "/buscar ");
    }
}

TEST_CASE("Enter en el menú completa o envía", "[command_menu]") {
    CommandMenu menu;
    SECTION("Un comando que necesita argumento no se envía vacío") {
        menu.update("/bus", true);
        const CommandMenuAction action = menu.handle(ListKey::Enter);
        CHECK(action.type == CommandMenuAction::Type::Complete);
        CHECK(action.text == "/buscar ");
    }
    SECTION("Con el nombre completo, también completa") {
        menu.update("/buscar", true);
        const CommandMenuAction action = menu.handle(ListKey::Enter);
        CHECK(action.type == CommandMenuAction::Type::Complete);
        CHECK(action.text == "/buscar ");
    }
    SECTION("Un comando sin argumento obligatorio se envía") {
        menu.update("/ex", true);
        const CommandMenuAction action = menu.handle(ListKey::Enter);
        CHECK(action.type == CommandMenuAction::Type::Submit);
        CHECK(action.text == "/exportar");
        CHECK_FALSE(menu.is_open());
        // Si el envío no vacía la caja (un aviso), el menú sigue cerrado.
        menu.update(action.text, true);
        CHECK_FALSE(menu.is_open());
    }
    SECTION("/copiar se envía sin el espacio") {
        menu.update("/", true);
        (void)menu.handle(ListKey::Down);
        const CommandMenuAction action = menu.handle(ListKey::Enter);
        CHECK(action.type == CommandMenuAction::Type::Submit);
        CHECK(action.text == "/copiar");
    }
}

TEST_CASE("Esc cierra el menú y no se reabre hasta que cambie el texto", "[command_menu]") {
    CommandMenu menu;
    menu.update("/g", true);
    const CommandMenuAction action = menu.handle(ListKey::Escape);
    CHECK(action.type == CommandMenuAction::Type::Handled);
    CHECK(action.text.empty());
    CHECK_FALSE(menu.is_open());

    // El mismo texto (cada cuadro vuelve a llamar update): sigue cerrado.
    menu.update("/g", true);
    CHECK_FALSE(menu.is_open());
    // Cerrado, Esc ya no es del menú (cancela la respuesta en main.cpp).
    CHECK(menu.handle(ListKey::Escape).type == CommandMenuAction::Type::Pass);
    // Perder el foco no olvida el cierre.
    menu.update("/g", false);
    menu.update("/g", true);
    CHECK_FALSE(menu.is_open());

    SECTION("Escribir otra letra lo abre") {
        menu.update("/gu", true);
        CHECK(menu.is_open());
    }
    SECTION("Borrar una letra también") {
        menu.update("/", true);
        CHECK(menu.is_open());
        // Y volver al texto del cierre ya no lo cierra.
        menu.update("/g", true);
        CHECK(menu.is_open());
    }
}

TEST_CASE("Ratón: hover mueve el cursor y clic completa", "[command_menu]") {
    CommandMenu menu;
    menu.update("/", true);
    menu.select(2);
    CHECK(at_cursor(menu) == "/guardar");
    menu.select(9); // Fuera de rango: no cambia.
    CHECK(at_cursor(menu) == "/guardar");

    const CommandMenuAction action = menu.choose(3);
    CHECK(action.type == CommandMenuAction::Type::Complete);
    CHECK(action.text == "/exportar");
    CHECK_FALSE(menu.is_open());
    CHECK(menu.choose(0).type == CommandMenuAction::Type::Pass);
}
