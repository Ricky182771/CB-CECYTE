#include "history_view.h"
#include "list_style.h"
#include "markdown.h"
#include "markdown_view.h"
#include "sidebar.h"
#include "sidebar_view.h"
#include "theme.h"

#include "chatbot/config.h"
#include "temp_dir.hpp"
#include "no_color_guard.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/color.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/terminal.hpp>

#include <cstdint>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using chatbot::cli::BackgroundMode;
using chatbot::cli::Entry;
using chatbot::cli::EntryKind;
using chatbot::cli::HistoryView;
using chatbot::cli::Palette;
using chatbot::cli::Theme;
using chatbot::cli::ThemeColor;
namespace md = chatbot::cli::md;

/// Los colores RGB se convierten según lo que soporte la terminal (FTXUI
/// v7.0.3, Color::Color): las pruebas fijan 24 bits para comparar RGB.
void true_color() { ftxui::Terminal::SetColorSupport(ftxui::Terminal::Color::TrueColor); }

const Theme& catppuccin() { return *chatbot::cli::find_theme("catppuccin-mocha"); }

ftxui::Color color_of(const ThemeColor& color) { return chatbot::cli::to_ftxui(color); }

/// Dibuja el elemento con el ancho dado y el alto que necesite.
ftxui::Screen draw(ftxui::Element element, int width) {
    ftxui::Screen screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(width),
                                                 ftxui::Dimension::Fit(element, true));
    ftxui::Render(screen, element);
    return screen;
}

std::string row_text(const ftxui::Screen& screen, int y) {
    std::string row;
    for (int x = 0; x < screen.dimx(); ++x) {
        row += screen.CellAt(x, y).character;
    }
    return row;
}

/// Primera celda donde empieza needle (en una sola fila), o {-1, -1}.
struct Position {
    int x = -1;
    int y = -1;
};
Position find(const ftxui::Screen& screen, std::string_view needle) {
    for (int y = 0; y < screen.dimy(); ++y) {
        for (int x = 0; x < screen.dimx(); ++x) {
            std::string text;
            for (int k = x; k < screen.dimx() && text.size() < needle.size(); ++k) {
                text += screen.CellAt(k, y).character;
            }
            if (text == needle) {
                return {x, y};
            }
        }
    }
    return {};
}

std::string read_file(const std::string& path) {
    std::ifstream file{path, std::ios::binary};
    std::ostringstream content;
    content << file.rdbuf();
    return content.str();
}

/// Markdown con todo lo que lleva color: resaltado, código, enlace, H3,
/// cita, alerta, tabla y bloque de código.
constexpr std::string_view kSample = R"(# Título

Texto con ==resaltado==, `código` y un [enlace](https://example.com).

### Subtítulo

> Una cita.

> [!WARNING]
> Cuidado.

| a | b |
|---|---|
| 1 | 2 |

```cpp
int x = 1;
```
)";

Entry entry(EntryKind kind, std::string text) {
    Entry e;
    e.kind = kind;
    e.text = std::move(text);
    return e;
}

std::vector<Entry> sample_entries() {
    Entry cancelled = entry(EntryKind::Assistant, "parcial");
    cancelled.cancelled = true;
    return {entry(EntryKind::User, "hola"), entry(EntryKind::Assistant, std::string{kSample}),
            cancelled, entry(EntryKind::Error, "Falló la conexión."),
            entry(EntryKind::Notice, "Aviso.")};
}

/// Los colores de texto de un tema (los que se revisan contra el fondo).
std::vector<std::pair<const char*, ThemeColor>> text_colors(const Theme& t) {
    return {
        {"text", t.text},
        {"heading_accent", t.heading_accent},
        {"inline_code", t.inline_code},
        {"link", t.link},
        {"user_label", t.user_label},
        {"assistant_label", t.assistant_label},
        {"error", t.error},
        {"notice", t.notice},
        {"alert_note", t.alert_note},
        {"alert_tip", t.alert_tip},
        {"alert_important", t.alert_important},
        {"alert_warning", t.alert_warning},
        {"alert_caution", t.alert_caution},
        {"input_placeholder", t.input_placeholder},
    };
}

/// Colores que no son texto pero deben distinguirse del fondo (≥ 3:1).
std::vector<std::pair<const char*, ThemeColor>> ui_colors(const Theme& t) {
    return {
        {"border", t.border},
        {"table_border", t.table_border},
        {"quote_bar", t.quote_bar},
        {"scrollbar", t.scrollbar},
        {"selection_bg", t.selection_bg},
    };
}

bool is_rgb(const ThemeColor& color) { return color.kind == ThemeColor::Kind::Rgb; }

} // namespace

TEST_CASE("Razón de contraste WCAG 2.x", "[tema]") {
    CHECK(chatbot::cli::contrast_ratio(0x000000, 0xFFFFFF) == Catch::Approx(21.0));
    CHECK(chatbot::cli::contrast_ratio(0xFFFFFF, 0x000000) == Catch::Approx(21.0));
    CHECK(chatbot::cli::contrast_ratio(0x1E1E2E, 0x1E1E2E) == Catch::Approx(1.0));
    CHECK(chatbot::cli::relative_luminance(0x000000) == Catch::Approx(0.0));
    CHECK(chatbot::cli::relative_luminance(0xFFFFFF) == Catch::Approx(1.0));
    // #777777 sobre blanco: 4.48 (justo debajo de 4.5).
    CHECK(chatbot::cli::contrast_ratio(0x777777, 0xFFFFFF) == Catch::Approx(4.48).margin(0.01));
    // Luminancia de cada canal por separado (coeficientes de WCAG).
    CHECK(chatbot::cli::relative_luminance(0xFF0000) == Catch::Approx(0.2126));
    CHECK(chatbot::cli::relative_luminance(0x00FF00) == Catch::Approx(0.7152));
    CHECK(chatbot::cli::relative_luminance(0x0000FF) == Catch::Approx(0.0722));
}

TEST_CASE("Tabla de temas: ids únicos, por defecto y búsqueda", "[tema]") {
    const auto all = chatbot::cli::themes();
    REQUIRE(all.size() >= 2);
    CHECK(&chatbot::cli::default_theme() == &all[0]);
    CHECK(chatbot::cli::default_theme().id == "catppuccin-mocha");
    std::set<std::string_view> ids;
    for (const Theme& theme : all) {
        CHECK(!theme.id.empty());
        CHECK(!theme.name.empty());
        CHECK(ids.insert(theme.id).second);
        CHECK(chatbot::cli::find_theme(theme.id) == &theme);
    }
    CHECK(chatbot::cli::find_theme("no-existe") == nullptr);
    const Theme* terminal = chatbot::cli::find_theme("terminal");
    REQUIRE(terminal != nullptr);
    CHECK(terminal->name == "De la terminal");
    CHECK_FALSE(chatbot::cli::has_own_background(*terminal));
    CHECK(chatbot::cli::has_own_background(catppuccin()));
}

TEST_CASE("Contraste de cada tema con fondo propio", "[tema]") {
    for (const Theme& theme : chatbot::cli::themes()) {
        if (!chatbot::cli::has_own_background(theme)) {
            continue;
        }
        INFO("tema " << theme.id);
        REQUIRE(is_rgb(theme.background));
        CHECK(theme.min_text_contrast >= 4.5);
        const std::uint32_t background = theme.background.value;
        for (const auto& [name, color] : text_colors(theme)) {
            INFO(name);
            REQUIRE(is_rgb(color)); // Con fondo propio, todo color es RGB.
            CHECK_FALSE(color.dim);
            CHECK(chatbot::cli::contrast_ratio(color.value, background) >=
                  theme.min_text_contrast);
        }
        REQUIRE(is_rgb(theme.selection_fg));
        REQUIRE(is_rgb(theme.selection_bg));
        REQUIRE(is_rgb(theme.highlight_fg));
        REQUIRE(is_rgb(theme.highlight_bg));
        CHECK(chatbot::cli::contrast_ratio(theme.selection_fg.value, theme.selection_bg.value) >=
              theme.min_text_contrast);
        CHECK(chatbot::cli::contrast_ratio(theme.highlight_fg.value, theme.highlight_bg.value) >=
              theme.min_text_contrast);
        for (const auto& [name, color] : ui_colors(theme)) {
            INFO(name);
            REQUIRE(is_rgb(color));
            CHECK(chatbot::cli::contrast_ratio(color.value, background) >= 3.0);
        }
    }
}

TEST_CASE("resolve_appearance: valores conocidos, vacíos y desconocidos", "[tema]") {
    const chatbot_test::NoColorGuard environment(nullptr);
    std::string warning = "x";
    auto appearance = chatbot::cli::resolve_appearance("", "", &warning);
    CHECK(appearance.theme == &chatbot::cli::default_theme());
    CHECK(appearance.background == BackgroundMode::FromTheme);
    CHECK(warning.empty());

    appearance = chatbot::cli::resolve_appearance("terminal", "terminal", &warning);
    CHECK(appearance.theme->id == "terminal");
    CHECK(appearance.background == BackgroundMode::Transparent);
    CHECK(warning.empty());

    appearance = chatbot::cli::resolve_appearance("no-existe", "raro", &warning);
    CHECK(appearance.theme == &chatbot::cli::default_theme());
    CHECK(appearance.background == BackgroundMode::FromTheme);
    CHECK(warning.find("no-existe") != std::string::npos);
    CHECK(warning.find("raro") != std::string::npos);
}

TEST_CASE("config.json con un tema desconocido cae al de por defecto", "[tema][config]") {
    const chatbot_test::NoColorGuard environment(nullptr);
    const chatbot_test::ScopedTempDir dir;
    const std::string config = (dir.path() / "config.json").string();
    {
        std::ofstream file{config};
        file << R"({"model": "m", "appearance": {"theme": "borrado", "background": "theme"}})";
    }
    const auto values = chatbot::load_appearance_values(config);
    REQUIRE(values.is_ok());
    std::string warning;
    const auto appearance =
        chatbot::cli::resolve_appearance(values.value().theme, values.value().background, &warning);
    CHECK(appearance.theme == &chatbot::cli::default_theme());
    CHECK_FALSE(warning.empty());
}

TEST_CASE("NO_COLOR: el cursor de modelos queda invertido sin alterar config.json", "[tema]") {
    const char* value = GENERATE(static_cast<const char*>(nullptr), "", "1", "0");
    const chatbot_test::NoColorGuard environment(value);
    const chatbot_test::ScopedTempDir dir;
    const std::string config = (dir.path() / "config.json").string();
    const std::string contents =
        R"({"appearance":{"theme":"catppuccin-mocha","background":"theme"}})";
    {
        std::ofstream file(config);
        file << contents;
    }
    const auto saved = chatbot::load_appearance_values(config);
    REQUIRE(saved.is_ok());
    const auto appearance = chatbot::cli::resolve_appearance(saved.value().theme,
                                                             saved.value().background);
    const bool disabled = value != nullptr && *value != '\0';
    CHECK(chatbot::cli::no_color_enabled() == disabled);
    CHECK(appearance.theme->id == (disabled ? "terminal" : "catppuccin-mocha"));
    const Palette palette(appearance);
    const auto screen = draw(chatbot::cli::choice_list({"modelo-a", "modelo-b"}, 1,
                                                       std::nullopt, true, palette), 30);
    const auto cursor = find(screen, "modelo-b");
    REQUIRE(cursor.x >= 0);
    CHECK(screen.CellAt(cursor.x, cursor.y).inverted == disabled);
    CHECK(read_file(config) == contents);
}

TEST_CASE("Fondo del tema: todas las celdas del historial lo llevan", "[tema]") {
    true_color();
    const Palette palette(catppuccin(), BackgroundMode::FromTheme);
    HistoryView view;
    const int width = 50;
    // Como en main.cpp: la paleta en la raíz, además de en cada entrada.
    ftxui::Screen screen = draw(view.render(sample_entries(), width, palette) | palette.base(),
                                width);
    const ftxui::Color background = color_of(catppuccin().background);
    const ftxui::Color highlight = color_of(catppuccin().highlight_bg);
    const Position mark = find(screen, "resaltado");
    REQUIRE(mark.x >= 0);
    int highlighted = 0;
    for (int y = 0; y < screen.dimy(); ++y) {
        for (int x = 0; x < screen.dimx(); ++x) {
            INFO("celda " << x << "," << y);
            const ftxui::Color cell = screen.CellAt(x, y).background_color;
            if (cell == highlight) {
                ++highlighted;
                CHECK(y == mark.y);
            } else {
                CHECK(cell == background);
            }
        }
    }
    CHECK(highlighted == 9); // "resaltado"
    // Sin dim ni inverted: con fondo propio, el tema pone los colores.
    for (int y = 0; y < screen.dimy(); ++y) {
        for (int x = 0; x < screen.dimx(); ++x) {
            CHECK_FALSE(screen.CellAt(x, y).dim);
            CHECK_FALSE(screen.CellAt(x, y).inverted);
        }
    }
    // El texto normal lleva el color de texto del tema.
    const Position hola = find(screen, "hola");
    REQUIRE(hola.x >= 0);
    CHECK(screen.CellAt(hola.x, hola.y).foreground_color == color_of(catppuccin().text));
}

TEST_CASE("Fondo del tema: la barra lateral solo cambia en la selección", "[tema]") {
    true_color();
    const Palette palette(catppuccin(), BackgroundMode::FromTheme);
    chatbot::cli::Sidebar sidebar;
    chatbot::cli::ConversationSummary item;
    item.id = "a";
    item.title = "Una conversación";
    item.updated_at = "2026-10-04T10:00:00-06:00";
    sidebar.open({item}, "a");
    const auto rows = sidebar.rows(chatbot::cli::CalendarDay{2026, 10, 4});
    const int width = 30;
    ftxui::Element element =
        ftxui::hbox({chatbot::cli::render_sidebar(sidebar, rows, width, 8, true, palette),
                     chatbot::cli::sidebar_divider(palette)}) |
        palette.base();
    ftxui::Screen screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(width + 1),
                                                 ftxui::Dimension::Fixed(8));
    ftxui::Render(screen, element);
    const ftxui::Color background = color_of(catppuccin().background);
    const ftxui::Color selection = color_of(catppuccin().selection_bg);
    int selected = 0;
    for (int y = 0; y < screen.dimy(); ++y) {
        for (int x = 0; x < screen.dimx(); ++x) {
            const ftxui::Color cell = screen.CellAt(x, y).background_color;
            if (cell == selection) {
                ++selected;
            } else {
                CHECK(cell == background);
            }
        }
    }
    CHECK(selected == width - 3); // Una fila, a todo lo ancho del contenido.
    // El borde, en el color border.
    CHECK(screen.CellAt(0, 0).foreground_color == color_of(catppuccin().border));
}

TEST_CASE("Fondo transparente: sin fondo salvo el resaltado; acentos en RGB", "[tema]") {
    true_color();
    const Palette palette(catppuccin(), BackgroundMode::Transparent);
    CHECK_FALSE(palette.paints_background());
    const int width = 60;
    ftxui::Screen screen =
        draw(md::render(md::parse(kSample), width, palette) | palette.base(), width);
    const ftxui::Color none = ftxui::Color(ftxui::Color::Default);
    const Position mark = find(screen, "resaltado");
    REQUIRE(mark.x >= 0);
    for (int y = 0; y < screen.dimy(); ++y) {
        for (int x = 0; x < screen.dimx(); ++x) {
            INFO("celda " << x << "," << y);
            const bool in_mark = y == mark.y && x >= mark.x && x < mark.x + 9;
            CHECK((screen.CellAt(x, y).background_color == none) != in_mark);
        }
    }
    const Position heading = find(screen, "Subtítulo");
    const Position code = find(screen, "código");
    const Position link = find(screen, "enlace");
    const Position text = find(screen, "Texto");
    REQUIRE(heading.x >= 0);
    REQUIRE(code.x >= 0);
    REQUIRE(link.x >= 0);
    REQUIRE(text.x >= 0);
    CHECK(screen.CellAt(heading.x, heading.y).foreground_color ==
          color_of(catppuccin().heading_accent));
    CHECK(screen.CellAt(code.x, code.y).foreground_color == color_of(catppuccin().inline_code));
    CHECK(screen.CellAt(link.x, link.y).foreground_color == color_of(catppuccin().link));
    CHECK(screen.CellAt(link.x, link.y).underlined);
    // El texto normal, con el color de la terminal.
    CHECK(screen.CellAt(text.x, text.y).foreground_color == none);
}

TEST_CASE("De la terminal: el render de la muestra es el de antes de los temas", "[tema]") {
    // Las capturas son Screen::ToString() del render con el código anterior
    // (md_preview en una terminal de 24 bits).
    true_color();
    const int width = GENERATE(40, 80);
    INFO("ancho " << width);
    const std::string sample = read_file(CHATBOT_TEST_DATA_DIR "/markdown_muestra.md");
    const std::string expected = read_file(CHATBOT_TEST_DATA_DIR "/markdown_muestra_terminal_w" +
                                           std::to_string(width) + ".ans");
    REQUIRE(!expected.empty());
    const Palette& palette = chatbot::cli::terminal_palette();
    const ftxui::Screen screen =
        draw(md::render(md::parse(sample), width, palette) | palette.base(), width);
    CHECK(screen.ToString() == expected);
}

TEST_CASE("Selección: cursor a todo lo ancho, elegido con ●, sin \">\"", "[tema]") {
    true_color();
    const Palette palette(catppuccin(), BackgroundMode::FromTheme);
    const std::vector<std::string> models = {"llama-3", "mistral", "qwen"};
    const int width = 30;
    const ftxui::Color selection = color_of(catppuccin().selection_bg);
    const auto list = [&](bool focused) {
        // El cursor en "mistral", el elegido en "qwen".
        ftxui::Element element =
            chatbot::cli::choice_list(models, 1, std::size_t{2}, focused, palette) | palette.base();
        ftxui::Screen screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(width),
                                                     ftxui::Dimension::Fixed(3));
        ftxui::Render(screen, element);
        return screen;
    };

    SECTION("Con el foco") {
        const ftxui::Screen screen = list(true);
        // La última columna es la de la barra de desplazamiento, fuera de las filas.
        for (int y = 0; y < screen.dimy(); ++y) {
            CHECK(screen.CellAt(0, y).character != ">");
            for (int x = 0; x < width - 1; ++x) {
                CHECK((screen.CellAt(x, y).background_color == selection) == (y == 1));
            }
            CHECK(screen.CellAt(width - 1, y).background_color != selection);
        }
        CHECK(screen.CellAt(2, 1).foreground_color == color_of(catppuccin().selection_fg));
        CHECK(row_text(screen, 2).rfind("● qwen", 0) == 0);
        CHECK(row_text(screen, 0).rfind("  llama-3", 0) == 0);
        // El elegido: "●" en heading_accent y negritas; las demás, texto normal.
        CHECK(screen.CellAt(0, 2).foreground_color == color_of(catppuccin().heading_accent));
        CHECK(screen.CellAt(0, 2).bold);
        CHECK(screen.CellAt(2, 0).foreground_color == color_of(catppuccin().text));
        CHECK_FALSE(screen.CellAt(2, 0).dim);
    }
    SECTION("Sin el foco") {
        const ftxui::Screen screen = list(false);
        for (int y = 0; y < screen.dimy(); ++y) {
            CHECK(screen.CellAt(0, y).character != ">");
            for (int x = 0; x < width; ++x) {
                CHECK(screen.CellAt(x, y).background_color != selection);
            }
        }
        CHECK(row_text(screen, 2).rfind("● qwen", 0) == 0);
    }
    SECTION("De la terminal: el cursor va invertido") {
        const Palette& terminal = chatbot::cli::terminal_palette();
        ftxui::Element element =
            chatbot::cli::choice_list(models, 0, std::nullopt, true, terminal);
        ftxui::Screen screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(width),
                                                     ftxui::Dimension::Fixed(3));
        ftxui::Render(screen, element);
        CHECK(screen.CellAt(0, 0).inverted);
        CHECK(screen.CellAt(width - 2, 0).inverted);
        CHECK_FALSE(screen.CellAt(0, 1).inverted);
        CHECK(row_text(screen, 0).rfind("  llama-3", 0) == 0);
    }
}

TEST_CASE("Cambiar el tema o el fondo invalida la caché del historial", "[tema][cache]") {
    HistoryView view;
    const std::vector<Entry> entries = sample_entries();
    const Palette with_background(catppuccin(), BackgroundMode::FromTheme);
    const Palette transparent(catppuccin(), BackgroundMode::Transparent);
    const Palette& terminal = chatbot::cli::terminal_palette();
    const std::size_t count = entries.size();

    (void)view.render(entries, 60, with_background);
    CHECK(view.draw_count() == count);
    (void)view.render(entries, 60, with_background);
    CHECK(view.draw_count() == count); // Igual: se reusa.
    (void)view.render(entries, 60, transparent);
    CHECK(view.draw_count() == 2 * count); // Otro fondo.
    (void)view.render(entries, 60, terminal);
    CHECK(view.draw_count() == 3 * count); // Otro tema.
    (void)view.render(entries, 60, terminal);
    CHECK(view.draw_count() == 3 * count);
    // El árbol de markdown no depende del tema: no se vuelve a parsear.
    CHECK(view.parse_count() == 2);
}
