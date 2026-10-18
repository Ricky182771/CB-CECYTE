// Símbolos Sencillos en las vistas: la barra lateral, el menú de comandos,
// un bloque de código con sus botones, el markdown, la barra de título y la
// línea de estado se dibujan igual que con Unicode, con los respaldos de
// glyph_table() en las mismas celdas (la configuración, en
// test_settings_screen.cpp).

#include "block_actions.h"
#include "code_blocks.h"
#include "command_menu.h"
#include "command_menu_view.h"
#include "commands.h"
#include "conversation.h"
#include "glyphs.h"
#include "history_view.h"
#include "markdown_view.h"
#include "sidebar.h"
#include "sidebar_view.h"
#include "status_line.h"
#include "symbols_check.hpp"
#include "theme.h"
#include "title_bar.h"

#include "chatbot/markdown/markdown.h"

#include <catch2/catch_test_macros.hpp>

#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/box.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/string.hpp>

#include <cstddef>
#include <string>
#include <vector>

namespace {

using chatbot::cli::Entry;
using chatbot::cli::EntryKind;
using chatbot::cli::HistoryView;
using chatbot::cli::Palette;
using chatbot::cli::Symbols;

ftxui::Screen draw(ftxui::Element element, int width, int height) {
    ftxui::Screen screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(width),
                                                 ftxui::Dimension::Fixed(height));
    ftxui::Render(screen, element);
    return screen;
}

ftxui::Screen draw_fit(ftxui::Element element, int width) {
    ftxui::Screen screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(width),
                                                 ftxui::Dimension::Fit(element, true));
    ftxui::Render(screen, element);
    return screen;
}

std::string screen_text(const ftxui::Screen& screen) {
    std::string text;
    for (int y = 0; y < screen.dimy(); ++y) {
        for (int x = 0; x < screen.dimx(); ++x) {
            const std::string& character = screen.CellAt(x, y).character;
            text += character.empty() ? std::string{" "} : character;
        }
        text += "\n";
    }
    return text;
}

Entry make(EntryKind kind, std::string text) {
    Entry entry;
    entry.kind = kind;
    entry.text = std::move(text);
    return entry;
}

chatbot::cli::ConversationSummary item(std::string id, std::string title) {
    chatbot::cli::ConversationSummary summary;
    summary.id = std::move(id);
    summary.title = std::move(title);
    summary.updated_at = "2026-10-04T10:00:00-06:00";
    return summary;
}

/// Dibuja con cada paleta en Unicode y en Sencillos, y compara. replaced:
/// si el dibujo Unicode debe llevar símbolos de la tabla (si no, ninguno).
template <typename Draw>
void check_all_palettes(const Draw& draw_with, bool replaced) {
    const std::vector<Palette> unicode = chatbot_test::palettes_with(Symbols::Unicode);
    const std::vector<Palette> simple = chatbot_test::palettes_with(Symbols::Simple);
    for (std::size_t i = 0; i < unicode.size(); ++i) {
        INFO("paleta " << unicode[i].key());
        const ftxui::Screen a = draw_with(unicode[i]);
        const ftxui::Screen b = draw_with(simple[i]);
        CHECK((chatbot_test::table_glyphs_in(a) > 0) == replaced);
        chatbot_test::check_simple_matches(a, b);
    }
}

} // namespace

TEST_CASE("símbolos sencillos: barra lateral", "[simbolos][barra]") {
    chatbot::cli::Sidebar sidebar;
    sidebar.open({item("a", "Recursividad en C++"), item("b", "Otra conversación")}, "a");
    const auto rows = sidebar.rows(chatbot::cli::CalendarDay{2026, 10, 4});
    const auto draw_with = [&](const Palette& palette) {
        return draw(ftxui::hbox({chatbot::cli::render_sidebar(sidebar, rows, 24, 10, true, palette),
                                 chatbot::cli::sidebar_divider(palette)}),
                    25, 10);
    };
    check_all_palettes(draw_with, true);
    const ftxui::Screen unicode = draw_with(Palette{});
    const ftxui::Screen simple =
        draw_with(Palette{chatbot::cli::default_theme(),
                          chatbot::cli::BackgroundMode::FromTheme, Symbols::Simple});
    CHECK(screen_text(unicode).find("⚙ Configuración") != std::string::npos);
    CHECK(screen_text(simple).find("* Configuración  (F2)") != std::string::npos);
    // La abierta: "●" falta en Lucida Console; con Sencillos, "•".
    CHECK(screen_text(unicode).find("● Recursividad") != std::string::npos);
    CHECK(screen_text(simple).find("• Recursividad") != std::string::npos);
}

TEST_CASE("símbolos sencillos: menú de comandos", "[simbolos][menu]") {
    chatbot::cli::CommandMenu menu;
    menu.update("/", true);
    const auto availability = chatbot::cli::command_availability(
        chatbot::cli::CommandContext{true, true, true, true, false});
    check_all_palettes([&](const Palette& palette) {
        return draw_fit(chatbot::cli::render_command_menu(menu, availability, 70, palette), 70);
    }, false);
}

TEST_CASE("símbolos sencillos: bloques de código con botones", "[simbolos][botones]") {
    const std::vector<Entry> entries{
        make(EntryKind::User, "dame código"),
        make(EntryKind::Assistant, "### Ejemplo\n\nTexto:\n\n- uno\n  - dos\n    - tres\n\n"
                                   "```cpp\nint a;\n```\n\n```\nsin lenguaje\n```")};
    for (const bool copied : {false, true}) {
        INFO("copiado: " << copied);
        check_all_palettes([&](const Palette& palette) {
            HistoryView view;
            if (copied) {
                view.show_copied(1);
            }
            ftxui::Element element = view.render(entries, 40, palette, {0, 1});
            ftxui::Screen screen = draw_fit(element, 40);
            // Los botones salen en el segundo cuadro (Picture::Render usa
            // el estado de la vista ya dibujada).
            element = view.render(entries, 40, palette, {0, 1});
            ftxui::Render(screen, element);
            return screen;
        }, true);
    }
    // Lo que se ve en cada modo.
    HistoryView view;
    view.show_copied(1);
    const Palette simple{chatbot::cli::default_theme(), chatbot::cli::BackgroundMode::FromTheme,
                         Symbols::Simple};
    (void)view.render(entries, 40, simple, {0, 1});
    const std::string text = screen_text(draw_fit(view.render(entries, 40, simple, {0, 1}), 40));
    CHECK(text.find("▌ Ejemplo") != std::string::npos);
    // Los tres niveles de viñeta se siguen distinguiendo.
    CHECK(text.find("\n• uno") != std::string::npos);
    CHECK(text.find("\n  o dos") != std::string::npos);
    CHECK(text.find("\n    - tres") != std::string::npos);
    CHECK(text.find("[√ Copiado] [Guardar]┐") != std::string::npos);
    CHECK(text.find("┌") != std::string::npos);
    CHECK(text.find("╭") == std::string::npos);
    CHECK(text.find("╯") == std::string::npos);
}

TEST_CASE("símbolos sencillos: tareas de markdown con [x] y la sangría medida",
          "[simbolos][markdown]") {
    const chatbot::md::Document document = chatbot::md::parse(
        "- [x] una tarea hecha con texto largo que se parte\n- [ ] pendiente\n\n"
        "1. [x] numerada");
    for (const Palette& palette : chatbot_test::palettes_with(Symbols::Simple)) {
        INFO("paleta " << palette.key());
        const std::string text =
            screen_text(draw_fit(chatbot::md::render(document, 24, palette), 24));
        CAPTURE(text);
        CHECK(text.find("☑") == std::string::npos);
        CHECK(text.find("☐") == std::string::npos);
        CHECK(text.find("• [x] una tarea hecha") == std::string::npos); // Sin viñeta extra.
        CHECK(text.find("[x] una tarea hecha con") != std::string::npos);
        // La línea siguiente se alinea con el texto, después de "[x] ".
        CHECK(text.find("\n    texto largo que se") != std::string::npos);
        CHECK(text.find("[ ] pendiente") != std::string::npos);
        CHECK(text.find("1. [x] numerada") != std::string::npos);
    }
    const std::string unicode = screen_text(draw_fit(chatbot::md::render(document, 24, Palette{}), 24));
    CHECK(unicode.find("☑ una tarea hecha con") != std::string::npos);
    CHECK(unicode.find("☐ pendiente") != std::string::npos);
}

TEST_CASE("símbolos sencillos: barra de título", "[simbolos][titulo]") {
    check_all_palettes([&](const Palette& palette) {
        ftxui::Box box;
        return draw_fit(chatbot::cli::title_bar("Conversación — larga", 60, palette,
                                                chatbot::cli::ExportButton{true, false}, box),
                        60);
    }, false);
}

TEST_CASE("símbolos sencillos: línea de estado con el aviso de un botón",
          "[simbolos][estado]") {
    chatbot::cli::Conversation conversation{""};
    REQUIRE(conversation.submit("dame código").has_value());
    conversation.append_delta("```cpp\nint a;");
    chatbot::cli::CodeBlockIndex index;
    index.update(conversation.entries());
    chatbot::cli::ClipboardAccess clipboard;
    chatbot::cli::ClipboardMethod wl_copy;
    wl_copy.kind = chatbot::cli::ClipboardMethod::Kind::Program;
    wl_copy.argv = {"wl-copy"};
    clipboard.methods = {wl_copy};
    clipboard.run = [](const std::vector<std::string>&, std::string_view) { return true; };
    const auto line = [&](Symbols symbols) {
        const chatbot::cli::ActionResult result = chatbot::cli::run_block_action(
            index.blocks(), 1, chatbot::cli::BlockAction::Copy, clipboard, std::nullopt, "",
            symbols);
        REQUIRE_FALSE(result.error);
        return draw(chatbot::cli::status_line({{result.message, ftxui::bold},
                                               {"↓ Hay más abajo (End)", ftxui::bold, true}},
                                              90),
                    90, 1);
    };
    const ftxui::Screen unicode = line(Symbols::Unicode);
    const ftxui::Screen simple = line(Symbols::Simple);
    CHECK(chatbot_test::table_glyphs_in(unicode) == 1);
    chatbot_test::check_simple_matches(unicode, simple);
    CHECK(screen_text(simple).rfind("! #1 incompleto (aún no termina) · copiado con wl-copy", 0) ==
          0);
}
