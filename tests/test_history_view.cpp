#include "code_blocks.h"
#include "conversation.h"
#include "history_view.h"
#include "performance.hpp"

#include <catch2/catch_test_macros.hpp>

#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/string.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace {

using chatbot::cli::Entry;
using chatbot::cli::EntryKind;
using chatbot::cli::HistoryView;

Entry make(EntryKind kind, std::string text) {
    Entry entry;
    entry.kind = kind;
    entry.text = std::move(text);
    return entry;
}

/// Texto de la pantalla completa (como lo deja ToString, sin estilos).
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

std::string draw(HistoryView& view, const std::vector<Entry>& entries, int width,
                 const std::vector<int>& first_code_numbers = {}) {
    ftxui::Element element =
        view.render(entries, width, chatbot::cli::terminal_palette(), first_code_numbers);
    ftxui::Screen screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(width),
                                                 ftxui::Dimension::Fit(element, true));
    ftxui::Render(screen, element);
    return screen_text(screen);
}

} // namespace

TEST_CASE("historial: el asistente se ve como markdown y el usuario como texto", "[historial]") {
    HistoryView view;
    const std::string text = draw(view,
                                  {make(EntryKind::User, "hola **no es markdown**"),
                                   make(EntryKind::Assistant, "# Título\n\n- uno\n- dos")},
                                  40);
    CHECK(text.find("Tú:") != std::string::npos);
    CHECK(text.find("hola **no es markdown**") != std::string::npos);
    CHECK(text.find("Asistente:") != std::string::npos);
    CHECK(text.find("# Título") == std::string::npos);
    CHECK(text.find("Título") != std::string::npos);
    CHECK(text.find("• uno") != std::string::npos);
}

TEST_CASE("historial: entradas planas filtradas", "[historial]") {
    HistoryView view;
    Entry assistant = make(EntryKind::Assistant, "ok");
    assistant.note = "fin\x1b[2J";
    const std::string text =
        draw(view,
             {make(EntryKind::User, "a\tb \x1b[31mrojo"), make(EntryKind::Error, "error\x07"),
              make(EntryKind::Notice, "aviso\x9b"), assistant},
             60);
    CHECK(text.find('\x1b') == std::string::npos);
    CHECK(text.find('\x07') == std::string::npos);
    CHECK(text.find("\xC2\x9B") == std::string::npos);
    CHECK(text.find("a    b") != std::string::npos); // Tab expandido.
    CHECK(text.find("\xEF\xBF\xBD[31mrojo") != std::string::npos);
    CHECK(text.find("fin\xEF\xBF\xBD[2J") != std::string::npos);
}

TEST_CASE("historial: solo vuelve a parsear lo que cambió", "[historial]") {
    HistoryView view;
    std::vector<Entry> entries{make(EntryKind::User, "pregunta"),
                               make(EntryKind::Assistant, "respuesta **uno**")};
    (void)view.render(entries, 80, chatbot::cli::terminal_palette());
    CHECK(view.parse_count() == 1); // Solo las del asistente se parsean.
    (void)view.render(entries, 80, chatbot::cli::terminal_palette());
    CHECK(view.draw_count() == 2); // El segundo cuadro reusa lo dibujado.
    (void)view.render(entries, 40, chatbot::cli::terminal_palette()); // Otro ancho: el árbol sirve igual.
    CHECK(view.parse_count() == 1);
    CHECK(view.draw_count() == 4);

    // Llega más texto del flujo: solo esa entrada se parsea de nuevo.
    entries.push_back(make(EntryKind::User, "otra"));
    entries.push_back(make(EntryKind::Assistant, "parcial"));
    (void)view.render(entries, 80, chatbot::cli::terminal_palette());
    CHECK(view.parse_count() == 2);
    entries.back().text += " y más";
    (void)view.render(entries, 80, chatbot::cli::terminal_palette());
    CHECK(view.parse_count() == 3);

    // Otra conversación con menos entradas: lo que cambió se parsea.
    const std::vector<Entry> other{make(EntryKind::Assistant, "distinta")};
    (void)view.render(other, 80, chatbot::cli::terminal_palette());
    CHECK(view.parse_count() == 4);
    (void)view.render(other, 80, chatbot::cli::terminal_palette());
    CHECK(view.parse_count() == 4);
}

TEST_CASE("historial: cuadro con 100 entradas y unos 200 KB", "[historial][desempeno]") {
    // Respuestas con todo tipo de bloques, como las de un modelo.
    const std::string answer =
        "## Respuesta\n\nTexto con **negritas**, *cursivas*, `código` y un "
        "[enlace](https://ej.com). Un párrafo de relleno que ocupa varias líneas en una "
        "terminal normal para parecerse a una respuesta real del modelo.\n\n"
        "- uno\n- dos\n  - dos.uno\n\n1. primero\n2. segundo\n\n"
        "| Columna | Valor | Nota |\n|:--|--:|:-:|\n| a | 1 | x |\n| b | 22 | y |\n\n"
        "```cpp\nint main() {\n    return 0; // comentario\n}\n```\n\n> una cita\n\n";
    std::vector<Entry> entries;
    std::size_t bytes = 0;
    while (entries.size() < 100) {
        std::string text;
        while (text.size() < 3900) {
            text += answer;
        }
        entries.push_back(make(EntryKind::User, "una pregunta de ejemplo"));
        entries.push_back(make(EntryKind::Assistant, text));
        bytes += text.size() + entries[entries.size() - 2].text.size();
    }
    CAPTURE(bytes);
    CHECK(bytes > 190 * 1024);

    const auto frame = [&](HistoryView& view) {
        const auto start = std::chrono::steady_clock::now();
        // Como en la interfaz: historial dentro de un frame vertical de 40 líneas.
        ftxui::Element element = view.render(entries, 100, chatbot::cli::terminal_palette()) | ftxui::focusPositionRelative(0.f, 1.f) |
                                 ftxui::yframe;
        ftxui::Screen screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(100),
                                                     ftxui::Dimension::Fixed(40));
        ftxui::Render(screen, element);
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
            .count();
    };
    HistoryView view;
    const double first = frame(view); // Incluye parsear todo.
    double steady = 0;
    constexpr int kFrames = 5;
    for (int i = 0; i < kFrames; ++i) {
        steady += frame(view);
    }
    steady /= kFrames;
    // Flujo en curso: solo cambia la última respuesta.
    entries.back().text += "\n\nmás texto **que llega**";
    const double streaming = frame(view);
    // Cambio de ancho (la terminal cambió de tamaño): todo se vuelve a dibujar.
    entries.front().text += " ";
    const auto resize_start = std::chrono::steady_clock::now();
    {
        ftxui::Element element = view.render(entries, 80, chatbot::cli::terminal_palette()) | ftxui::yframe;
        ftxui::Screen screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(80),
                                                     ftxui::Dimension::Fixed(40));
        ftxui::Render(screen, element);
    }
    const double resize =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - resize_start)
            .count();
    WARN("100 entradas, " << bytes / 1024 << " KB: primer cuadro " << first
                          << " ms; cuadros siguientes " << steady << " ms (promedio); con "
                          << "la última respuesta cambiando " << streaming
                          << " ms; cambio de ancho " << resize << " ms");
    CHECK(view.parse_count() == 51);
    if (chatbot_test::strict_performance()) {
        CHECK(steady < 50.0);
        CHECK(streaming < 50.0);
    }
}

TEST_CASE("historial: los bordes no se fusionan con lo que lo rodea", "[historial]") {
    // Una cita al final queda pegada al separador de la interfaz: la barra
    // "│" no debe convertir el separador en "┴".
    HistoryView view;
    ftxui::Element element =
        ftxui::vbox({view.render({make(EntryKind::Assistant, "> cita")}, 20, chatbot::cli::terminal_palette()), ftxui::separator()});
    ftxui::Screen screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(20),
                                                 ftxui::Dimension::Fit(element, true));
    ftxui::Render(screen, element);
    REQUIRE(screen.dimy() == 3);
    CHECK(screen.CellAt(0, 1).character == "│");
    CHECK(screen.CellAt(0, 2).character == "─");
}

TEST_CASE("historial: el estilo de etiquetas y texto plano no se estira", "[historial]") {
    Entry cancelled = make(EntryKind::Assistant, "parcial");
    cancelled.cancelled = true;
    Entry noted = make(EntryKind::Assistant, "ok");
    noted.note = "La respuesta se cortó por el límite de longitud.";
    HistoryView view;
    ftxui::Element element = view.render({make(EntryKind::User, "hola"), cancelled, noted,
                                          make(EntryKind::Error, "Falló la conexión."),
                                          make(EntryKind::Notice, "Aviso corto.")},
                                         40, chatbot::cli::terminal_palette());
    ftxui::Screen screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(40),
                                                 ftxui::Dimension::Fit(element, true));
    ftxui::Render(screen, element);
    for (int y = 0; y < screen.dimy(); ++y) {
        int last = -1;
        for (int x = 0; x < screen.dimx(); ++x) {
            const std::string& character = screen.CellAt(x, y).character;
            if (!character.empty() && character != " ") {
                last = x;
            }
        }
        for (int x = last + 1; x < screen.dimx(); ++x) {
            CAPTURE(x, y);
            const ftxui::Cell& cell = screen.CellAt(x, y);
            CHECK_FALSE(cell.bold);
            CHECK_FALSE(cell.dim);
            CHECK(cell.foreground_color == ftxui::Color(ftxui::Color::Default));
        }
    }
    // El estilo sí está en el texto.
    const std::string text = screen_text(screen);
    CHECK(text.find("Falló la conexión.") != std::string::npos);
    CHECK(screen.CellAt(0, 0).bold); // "Tú:"
}

TEST_CASE("historial: fuentes de una búsqueda con hipervínculos y texto filtrado",
          "[historial][busqueda]") {
    Entry sources;
    sources.kind = EntryKind::Sources;
    sources.sources = {
        {"Crónica del partido", "https://ejemplo.com/cronica", "contenido", ""},
        {"", "https://ejemplo.com/sin-titulo", "contenido", ""},
        {"Malo\x1b[31m\nrojo", "https://ejemplo.com/a b\x07", "contenido", ""},
    };
    HistoryView view;
    ftxui::Element element = view.render({sources}, 80, chatbot::cli::terminal_palette());
    ftxui::Screen screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(80),
                                                 ftxui::Dimension::Fit(element, true));
    ftxui::Render(screen, element);
    const std::string text = screen_text(screen);

    CHECK(text.find("Fuentes:") != std::string::npos);
    CHECK(text.find("[1] Crónica del partido (https://ejemplo.com/cronica)") !=
          std::string::npos);
    // Sin título, la URL una sola vez.
    CHECK(text.find("[2] https://ejemplo.com/sin-titulo ") != std::string::npos);
    CHECK(text.find("(https://ejemplo.com/sin-titulo)") == std::string::npos);
    // ESC y BEL no llegan a la pantalla; el salto de línea del título es un espacio.
    CHECK(text.find('\x1b') == std::string::npos);
    CHECK(text.find('\x07') == std::string::npos);
    CHECK(text.find("[3] Malo") != std::string::npos);

    // Los enlaces salen de la búsqueda, codificados para OSC 8.
    std::set<std::string> links;
    for (int y = 0; y < screen.dimy(); ++y) {
        for (int x = 0; x < screen.dimx(); ++x) {
            const ftxui::Cell& cell = screen.CellAt(x, y);
            if (cell.hyperlink != 0) {
                links.insert(screen.Hyperlink(cell.hyperlink));
            }
        }
    }
    CHECK(links.count("https://ejemplo.com/cronica") == 1);
    CHECK(links.count("https://ejemplo.com/sin-titulo") == 1);
    REQUIRE(links.size() == 3);
    for (const std::string& link : links) {
        for (const char c : link) {
            const auto byte = static_cast<unsigned char>(c);
            CHECK((byte >= 0x21 && byte <= 0x7E));
        }
    }
}

TEST_CASE("historial: fuentes con y sin fecha de publicación", "[historial][busqueda]") {
    Entry sources;
    sources.kind = EntryKind::Sources;
    sources.sources = {
        {"Con fecha", "https://ejemplo.com/1", "c", "2026-10-04"},
        {"Sin fecha", "https://ejemplo.com/2", "c", ""},
        {"", "https://ejemplo.com/3", "c", "2026-10-05"},
    };
    HistoryView view;
    const std::string text = draw(view, {sources}, 80);
    CHECK(text.find("[1] Con fecha — 2026-10-04 (https://ejemplo.com/1)") != std::string::npos);
    CHECK(text.find("[2] Sin fecha (https://ejemplo.com/2)") != std::string::npos);
    CHECK(text.find("[3] https://ejemplo.com/3 — 2026-10-05") != std::string::npos);
    CHECK(text.find("(https://ejemplo.com/3)") == std::string::npos);
}

TEST_CASE("historial: una fuente con URL que no es web es texto sin enlace",
          "[historial][busqueda]") {
    Entry sources;
    sources.kind = EntryKind::Sources;
    sources.sources = {
        {"Malo", "javascript:alert(1)", "c", "2026-10-04"},
        {"", "file:///etc/passwd", "c", ""},
        {"Bueno", "https://ejemplo.com/1", "c", ""},
    };
    HistoryView view;
    ftxui::Element element = view.render({sources}, 80, chatbot::cli::terminal_palette());
    ftxui::Screen screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(80),
                                                 ftxui::Dimension::Fit(element, true));
    ftxui::Render(screen, element);
    const std::string text = screen_text(screen);
    CHECK(text.find("[1] Malo — 2026-10-04 (javascript:alert(1))") != std::string::npos);
    CHECK(text.find("[2] file:///etc/passwd") != std::string::npos);
    CHECK(text.find("[3] Bueno (https://ejemplo.com/1)") != std::string::npos);

    std::set<std::string> links;
    for (int y = 0; y < screen.dimy(); ++y) {
        for (int x = 0; x < screen.dimx(); ++x) {
            const ftxui::Cell& cell = screen.CellAt(x, y);
            if (cell.hyperlink != 0) {
                links.insert(screen.Hyperlink(cell.hyperlink));
            }
        }
    }
    CHECK(links == std::set<std::string>{"https://ejemplo.com/1"});
    // Sin subrayado: no parece enlace (el enlace válido sí lo lleva).
    bool good_underlined = false;
    for (int x = 0; x < screen.dimx(); ++x) {
        CHECK_FALSE(screen.CellAt(x, 1).underlined);
        CHECK_FALSE(screen.CellAt(x, 2).underlined);
        good_underlined = good_underlined || screen.CellAt(x, 3).underlined;
    }
    CHECK(good_underlined);
}

TEST_CASE("historial: la caché vuelve a dibujar si cambian las fuentes",
          "[historial][busqueda]") {
    Entry sources;
    sources.kind = EntryKind::Sources;
    sources.sources = {{"Uno", "https://ejemplo.com/1", "c", ""}};
    HistoryView view;
    (void)draw(view, {sources}, 40);
    const std::size_t drawn = view.draw_count();
    (void)draw(view, {sources}, 40);
    CHECK(view.draw_count() == drawn);
    sources.sources[0].url = "https://ejemplo.com/2";
    std::string text = draw(view, {sources}, 40);
    CHECK(view.draw_count() == drawn + 1);
    CHECK(text.find("https://ejemplo.com/2") != std::string::npos);
    sources.sources[0].published_date = "2026-10-04";
    text = draw(view, {sources}, 40);
    CHECK(view.draw_count() == drawn + 2);
    CHECK(text.find("2026-10-04") != std::string::npos);
}

TEST_CASE("historial: los bloques de código llevan el número de collect_code_blocks",
          "[historial][code_blocks]") {
    std::vector<Entry> entries{
        make(EntryKind::User, "```cpp\nno cuenta\n```"),
        make(EntryKind::Assistant, "```cpp\nint a;\n```\n\n```\nsin lenguaje\n```"),
        make(EntryKind::Notice, "aviso"),
        make(EntryKind::Assistant,
             "- lista\n\n  ```py\n  print(1)\n  ```\n\n> ```sh\n> ls\n> ```"),
    };
    const auto numbered = [&](HistoryView& view) {
        const std::vector<chatbot::cli::CodeBlock> blocks =
            chatbot::cli::collect_code_blocks(entries);
        const std::string text =
            draw(view, entries, 40, chatbot::cli::first_code_numbers(blocks, entries.size()));
        // Cada bloque aparece con su número y en el mismo orden.
        std::size_t from = 0;
        for (const chatbot::cli::CodeBlock& block : blocks) {
            const std::string title = " #" + std::to_string(block.number) +
                                      (block.info.empty() ? "" : " · " + block.info) + " ";
            INFO(title);
            const std::size_t at = text.find(title, from);
            CHECK(at != std::string::npos);
            from = at == std::string::npos ? from : at;
        }
        return text;
    };
    HistoryView view;
    std::string text = numbered(view);
    CHECK(text.find("```cpp") != std::string::npos); // El del usuario no es markdown.
    CHECK(text.find("#5") == std::string::npos);

    // Una respuesta en curso agrega números sin cambiar los anteriores.
    entries.push_back(make(EntryKind::User, "más"));
    entries.push_back(make(EntryKind::Assistant, "```go\nfunc"));
    entries.back().in_progress = true;
    text = numbered(view);
    CHECK(text.find(" #5 · go ") != std::string::npos);

    // Sin números (vector vacío): el título de siempre.
    HistoryView plain;
    text = draw(plain, entries, 40);
    CHECK(text.find("╭ cpp ─") != std::string::npos);
    CHECK(text.find("#1") == std::string::npos);
}

TEST_CASE("historial: el número del primer bloque es parte de la caché", "[historial][code_blocks]") {
    const std::vector<Entry> entries{make(EntryKind::Assistant, "```cpp\nint a;\n```")};
    HistoryView view;
    CHECK(draw(view, entries, 40, {1}).find(" #1 · cpp ") != std::string::npos);
    CHECK(view.draw_count() == 1);
    CHECK(draw(view, entries, 40, {1}).find(" #1 · cpp ") != std::string::npos);
    CHECK(view.draw_count() == 1);
    // Cambió el número (por ejemplo, se borró una entrada anterior).
    CHECK(draw(view, entries, 40, {4}).find(" #4 · cpp ") != std::string::npos);
    CHECK(view.draw_count() == 2);
    CHECK(view.parse_count() == 1); // El árbol sirve igual.
}

TEST_CASE("historial: guarda la caja, el número y el título de cada bloque",
          "[historial][code_blocks]") {
    const std::vector<Entry> entries{
        make(EntryKind::Assistant, "Uno:\n\n```cpp\nint a;\n```\n\n- ```\n  x\n  ```"),
        make(EntryKind::User, "hola"),
    };
    HistoryView view;
    ftxui::Element element = view.render(entries, 40, chatbot::cli::terminal_palette(), {3, 5});
    ftxui::Screen screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(40),
                                                 ftxui::Dimension::Fit(element, true));
    ftxui::Render(screen, element);
    const std::vector<chatbot::cli::CodeFrame>& frames = view.code_frames(0);
    REQUIRE(frames.size() == 2);
    CHECK(frames[0].number == 3);
    CHECK(frames[1].number == 4);
    CHECK(frames[0].title_width == ftxui::string_width(" #3 · cpp "));
    CHECK(frames[1].title_width == ftxui::string_width(" #4 "));
    // La primera entrada empieza en la fila 0: sus coordenadas son las de la pantalla.
    for (const chatbot::cli::CodeFrame& frame : frames) {
        CHECK(screen.CellAt(frame.box.x_min, frame.box.y_min).character == "╭");
        CHECK(screen.CellAt(frame.box.x_max, frame.box.y_max).character == "╯");
    }
    CHECK(view.code_frames(1).empty());
    CHECK(view.code_frames(7).empty());

    // Sin números, sin cajas.
    HistoryView plain;
    (void)plain.render(entries, 40, chatbot::cli::terminal_palette());
    CHECK(plain.code_frames(0).empty());
}

namespace {

/// Dibuja el historial en una vista de height filas cuya primera fila
/// visible es top, como Scroll en main.cpp (focusPosition + yframe).
ftxui::Screen draw_view(HistoryView& view, const std::vector<Entry>& entries, int width,
                        int height, int top, const std::vector<int>& numbers) {
    ftxui::Element element = view.render(entries, width, chatbot::cli::terminal_palette(),
                                         numbers) |
                             ftxui::focusPosition(0, top + (height - 1) / 2) | ftxui::yframe;
    ftxui::Screen screen(width, height);
    ftxui::Render(screen, element);
    return screen;
}

/// Texto de la fila y (una celda vacía cuenta como espacio).
std::string row_text(const ftxui::Screen& screen, int y) {
    std::string text;
    for (int x = 0; x < screen.dimx(); ++x) {
        const std::string& character = screen.CellAt(x, y).character;
        text += character.empty() ? std::string{" "} : character;
    }
    return text;
}

/// Filas de la pantalla donde aparece needle.
std::vector<int> rows_with(const ftxui::Screen& screen, const std::string& needle) {
    std::vector<int> rows;
    for (int y = 0; y < screen.dimy(); ++y) {
        if (row_text(screen, y).find(needle) != std::string::npos) {
            rows.push_back(y);
        }
    }
    return rows;
}

/// Columna (en celdas) donde empieza needle en la fila y, o -1.
int column_of(const ftxui::Screen& screen, int y, const std::string& needle) {
    for (int x = 0; x < screen.dimx(); ++x) {
        std::string text;
        for (int k = x; k < screen.dimx() && text.size() < needle.size(); ++k) {
            const std::string& character = screen.CellAt(k, y).character;
            text += character.empty() ? std::string{" "} : character;
        }
        if (text.rfind(needle, 0) == 0) {
            return x;
        }
    }
    return -1;
}

bool same_cell(const ftxui::Cell& a, const ftxui::Cell& b) {
    return a.character == b.character && a.foreground_color == b.foreground_color &&
           a.background_color == b.background_color && a.bold == b.bold && a.dim == b.dim &&
           a.inverted == b.inverted && a.underlined == b.underlined;
}

/// Conversación con un bloque de 40 líneas entre texto, para recorrer todos
/// los desplazamientos de una vista de 10 filas.
std::vector<Entry> long_block_entries() {
    std::string code;
    for (int i = 1; i <= 40; ++i) {
        code += "linea " + std::to_string(i) + "\n";
    }
    std::string before;
    for (int i = 0; i < 12; ++i) {
        before += "antes " + std::to_string(i) + "\n\n";
    }
    std::string after;
    for (int i = 0; i < 12; ++i) {
        after += "\n\ndespues " + std::to_string(i);
    }
    return {make(EntryKind::User, "hola"),
            make(EntryKind::Assistant, before + "```cpp\n" + code + "```" + after)};
}

/// Para cada celda de la pantalla, hit_test da el botón cuya etiqueta se
/// dibujó ahí ("[Copiar]" o "[Guardar]", del bloque en el orden de numbers),
/// y nada en las demás celdas.
void check_hits(const HistoryView& view, const ftxui::Screen& screen,
                const std::vector<int>& numbers) {
    using chatbot::cli::BlockAction;
    struct Expected {
        BlockAction action;
    };
    std::vector<std::vector<std::optional<Expected>>> expected(
        static_cast<std::size_t>(screen.dimy()),
        std::vector<std::optional<Expected>>(static_cast<std::size_t>(screen.dimx())));
    for (int y = 0; y < screen.dimy(); ++y) {
        // Cada fila tiene a lo más los botones de un bloque.
        for (const auto& [label, action] :
             {std::pair{std::string{"[Copiar]"}, BlockAction::Copy},
              std::pair{std::string{"[Guardar]"}, BlockAction::Save}}) {
            const int x = column_of(screen, y, label);
            if (x < 0) {
                continue;
            }
            for (int k = 0; k < ftxui::string_width(label); ++k) {
                expected[static_cast<std::size_t>(y)][static_cast<std::size_t>(x + k)] =
                    Expected{action};
            }
        }
    }
    for (int y = 0; y < screen.dimy(); ++y) {
        for (int x = 0; x < screen.dimx(); ++x) {
            INFO("celda (" << x << ", " << y << ")");
            const std::optional<chatbot::cli::ButtonHit> hit = view.hit_test(x, y);
            const std::optional<Expected>& want =
                expected[static_cast<std::size_t>(y)][static_cast<std::size_t>(x)];
            REQUIRE(hit.has_value() == want.has_value());
            if (hit.has_value()) {
                CHECK(hit->action == want->action);
                CHECK(std::find(numbers.begin(), numbers.end(), hit->block) != numbers.end());
            }
        }
    }
}

} // namespace

TEST_CASE("botones: fijos arriba del bloque en todos los desplazamientos",
          "[historial][botones]") {
    const std::vector<Entry> entries = long_block_entries();
    const std::vector<int> numbers{1, 1};
    constexpr int kWidth = 50;
    constexpr int kHeight = 10;
    // Dónde queda el bloque en el historial completo.
    HistoryView full;
    ftxui::Element whole = full.render(entries, kWidth, chatbot::cli::terminal_palette(), numbers);
    ftxui::Screen all = ftxui::Screen::Create(ftxui::Dimension::Fixed(kWidth),
                                              ftxui::Dimension::Fit(whole, true));
    ftxui::Render(all, whole);
    const std::vector<int> tops = rows_with(all, "╭ #1 · cpp ");
    REQUIRE(tops.size() == 1);
    const int block_top = tops.front();
    const std::vector<chatbot::cli::CodeFrame>& frames = full.code_frames(1);
    REQUIRE(frames.size() == 1);
    const int block_bottom = block_top + frames[0].box.y_max - frames[0].box.y_min;
    REQUIRE(all.CellAt(frames[0].box.x_max, block_bottom).character == "╯");
    const int max_top = all.dimy() - kHeight;
    REQUIRE(block_top > kHeight);              // Al principio, el bloque está abajo.
    REQUIRE(block_bottom < max_top);           // Al final, ya pasó.
    const std::string labels = "[Copiar] [Guardar]";
    const int label_x = frames[0].box.x_max - ftxui::string_width(labels);

    HistoryView view;
    HistoryView plain;
    plain.set_buttons_visible(false);
    int on_border = 0;
    int pinned = 0;
    int at_end = 0;
    int only_bottom = 0;
    int hidden = 0;
    for (int top = 0; top <= max_top; ++top) {
        INFO("primera fila visible: " << top);
        const ftxui::Screen screen = draw_view(view, entries, kWidth, kHeight, top, numbers);
        const std::vector<int> rows = rows_with(screen, labels);
        const bool top_visible = block_top >= top && block_top < top + kHeight;
        if (top_visible) {
            // Con el borde superior a la vista, sobre el borde.
            REQUIRE(rows.size() == 1);
            CHECK(rows.front() == block_top - top);
            CHECK(row_text(screen, rows.front()).find("╭ #1 · cpp ") != std::string::npos);
            ++on_border;
        } else if (block_top < top && top <= block_bottom - 1) {
            // Fijos en la primera fila visible del contenido.
            REQUIRE(rows.size() == 1);
            CHECK(rows.front() == 0);
            CHECK(screen.CellAt(frames[0].box.x_min, 0).character == "│");
            ++pinned;
            if (top == block_bottom - 1) {
                ++at_end; // La fila bottom - 1: la última de contenido.
            }
        } else {
            CHECK(rows.empty());
            if (top == block_bottom) {
                CHECK(screen.CellAt(frames[0].box.x_max, 0).character == "╯");
                ++only_bottom;
            } else {
                ++hidden;
            }
        }
        if (!rows.empty()) {
            // Terminan una columna antes del borde derecho, sin tapar la esquina.
            CHECK(column_of(screen, rows.front(), labels) == label_x);
            CHECK(screen.CellAt(frames[0].box.x_max, rows.front()).character ==
                  (top_visible ? "╮" : "│"));
        }
        // hit_test coincide con las celdas dibujadas.
        check_hits(view, screen, {1});
        // Fuera de la caja del bloque, todo igual que sin botones.
        const ftxui::Screen without = draw_view(plain, entries, kWidth, kHeight, top, numbers);
        for (int y = 0; y < kHeight; ++y) {
            const int content_y = top + y;
            const bool inside_block = content_y >= block_top && content_y <= block_bottom;
            for (int x = 0; x < kWidth; ++x) {
                const bool inside = inside_block && x >= frames[0].box.x_min &&
                                    x <= frames[0].box.x_max;
                if (!inside) {
                    INFO("celda (" << x << ", " << y << ")");
                    CHECK(same_cell(screen.CellAt(x, y), without.CellAt(x, y)));
                }
            }
        }
    }
    CHECK(on_border > 0);
    CHECK(pinned > 0);
    CHECK(at_end == 1);
    CHECK(only_bottom == 1);
    CHECK(hidden > 0);
}

TEST_CASE("botones: completos, compactos u ocultos según el ancho", "[historial][botones]") {
    const std::vector<Entry> entries{make(EntryKind::Assistant, "```cpp\nint a;\n```")};
    // Título " #1 · cpp " (10 columnas) desde la columna 1, 2 de margen y los
    // botones hasta una columna antes del borde.
    const auto first_row = [&](int width) {
        HistoryView view;
        return row_text(draw_view(view, entries, width, 5, 0, {1}), 1);
    };
    CHECK(first_row(32).find("╭ #1 · cpp ──[Copiar] [Guardar]╮") != std::string::npos);
    CHECK(first_row(31).find("[Copiar]") == std::string::npos);
    CHECK(first_row(31).find("╭ #1 · cpp ────────────[C] [G]╮") != std::string::npos);
    CHECK(first_row(21).find("╭ #1 · cpp ──[C] [G]╮") != std::string::npos);
    CHECK(first_row(20).find("[C]") == std::string::npos);
    CHECK(first_row(20).find("╭ #1 · cpp ────────╮") != std::string::npos);
}

TEST_CASE("botones: solo en bloques numerados", "[historial][botones]") {
    const std::vector<Entry> entries{make(EntryKind::Assistant, "```cpp\nint a;\n```")};
    HistoryView view;
    CHECK(rows_with(draw_view(view, entries, 40, 5, 0, {}), "[Copiar]").empty());
    CHECK(rows_with(draw_view(view, entries, 40, 5, 0, {1}), "[Copiar]").size() == 1);
}

TEST_CASE("botones: el puntero encima no vuelve a dibujar la entrada", "[historial][botones]") {
    const std::vector<Entry> entries{make(EntryKind::Assistant, "```cpp\nint a;\n```")};
    HistoryView view;
    ftxui::Screen screen = draw_view(view, entries, 40, 5, 0, {1});
    const std::size_t draws = view.draw_count();
    const int copy_x = column_of(screen, 1, "[Copiar]");
    const int save_x = column_of(screen, 1, "[Guardar]");
    REQUIRE(copy_x > 0);
    CHECK_FALSE(screen.CellAt(copy_x, 1).inverted);

    view.set_hover(copy_x + 2, 1);
    screen = draw_view(view, entries, 40, 5, 0, {1});
    for (int x = copy_x; x < copy_x + 8; ++x) {
        CHECK(screen.CellAt(x, 1).inverted); // "De la terminal": selección invertida.
    }
    CHECK_FALSE(screen.CellAt(copy_x + 8, 1).inverted); // El espacio entre botones.
    CHECK_FALSE(screen.CellAt(save_x, 1).inverted);

    view.set_hover(save_x, 1);
    screen = draw_view(view, entries, 40, 5, 0, {1});
    CHECK_FALSE(screen.CellAt(copy_x, 1).inverted);
    CHECK(screen.CellAt(save_x, 1).inverted);

    view.clear_hover();
    screen = draw_view(view, entries, 40, 5, 0, {1});
    CHECK_FALSE(screen.CellAt(save_x, 1).inverted);
    CHECK(view.draw_count() == draws);
}

TEST_CASE("botones: hit_test con dos bloques en la misma entrada", "[historial][botones]") {
    std::string first;
    for (int i = 0; i < 6; ++i) {
        first += "a" + std::to_string(i) + "\n";
    }
    const std::vector<Entry> entries{
        make(EntryKind::Assistant, "```cpp\n" + first + "```\n\n```py\n" + first + "```")};
    HistoryView view;
    // Los dos bloques miden 8 filas cada uno, con una línea en blanco entre ellos.
    for (int top = 0; top <= 12; ++top) {
        INFO("primera fila visible: " << top);
        const ftxui::Screen screen = draw_view(view, entries, 40, 6, top, {4});
        check_hits(view, screen, {4, 5});
        // Cada fila de botones es del bloque que la contiene.
        const std::vector<chatbot::cli::CodeFrame>& frames = view.code_frames(0);
        REQUIRE(frames.size() == 2);
        for (int y = 0; y < 6; ++y) {
            const int x = column_of(screen, y, "[Copiar]");
            if (x < 0) {
                continue;
            }
            const std::optional<chatbot::cli::ButtonHit> hit = view.hit_test(x, y);
            REQUIRE(hit.has_value());
            const int content_y = top + y;
            const auto& frame = content_y <= frames[0].box.y_max ? frames[0] : frames[1];
            CHECK(hit->block == frame.number);
            // La única entrada empieza en la fila 0: sus coordenadas son las
            // del historial. Nunca sobre el borde inferior.
            CHECK(content_y >= frame.box.y_min);
            CHECK(content_y <= frame.box.y_max - 1);
        }
    }
    // Con todo a la vista, dos filas de botones, de los bloques 4 y 5.
    const ftxui::Screen screen = draw_view(view, entries, 40, 30, 0, {4});
    const std::vector<int> rows = rows_with(screen, "[Copiar] [Guardar]");
    REQUIRE(rows.size() == 2);
    CHECK(view.hit_test(column_of(screen, rows[0], "[Copiar]"), rows[0])->block == 4);
    CHECK(view.hit_test(column_of(screen, rows[1], "[Guardar]"), rows[1])->block == 5);
    CHECK(view.hit_test(column_of(screen, rows[1], "[Guardar]"), rows[1])->action ==
          chatbot::cli::BlockAction::Save);
    CHECK_FALSE(view.hit_test(column_of(screen, rows[0], "[Copiar]") + 8, rows[0]).has_value());
}

TEST_CASE("botones: [✓ Copiado] hasta que el puntero sale o pasan 2 s", "[historial][botones]") {
    const std::vector<Entry> entries{
        make(EntryKind::Assistant, "```cpp\nint a;\n```\n\n```py\nx = 1\n```")};
    auto now = std::chrono::steady_clock::time_point{};
    HistoryView view([&now] { return now; });
    ftxui::Screen screen = draw_view(view, entries, 40, 10, 0, {1});
    const std::size_t draws = view.draw_count();
    const std::vector<int> rows = rows_with(screen, "[Copiar] [Guardar]");
    REQUIRE(rows.size() == 2);
    const int copy_x = column_of(screen, rows[0], "[Copiar]");

    // Clic en copiar del bloque 1, con el puntero encima.
    view.set_hover(copy_x, rows[0]);
    view.show_copied(1);
    screen = draw_view(view, entries, 40, 10, 0, {1});
    CHECK(row_text(screen, rows[0]).find("[✓ Copiado] [Guardar]╮") != std::string::npos);
    CHECK(row_text(screen, rows[1]).find("[Copiar] [Guardar]╮") != std::string::npos);
    // El botón creció a la izquierda: el puntero sigue encima.
    const std::optional<chatbot::cli::ButtonHit> hit = view.hit_test(copy_x, rows[0]);
    REQUIRE(hit.has_value());
    CHECK(hit->action == chatbot::cli::BlockAction::Copy);
    CHECK(view.hit_test(column_of(screen, rows[0], "[✓ Copiado]"), rows[0]).has_value());

    // Moverse dentro del botón no lo quita; 1.9 s después sigue.
    view.set_hover(copy_x + 1, rows[0]);
    now += std::chrono::milliseconds{1900};
    screen = draw_view(view, entries, 40, 10, 0, {1});
    CHECK(row_text(screen, rows[0]).find("[✓ Copiado]") != std::string::npos);
    // A los 2 s, en el siguiente cuadro, vuelve a [Copiar].
    now += std::chrono::milliseconds{100};
    screen = draw_view(view, entries, 40, 10, 0, {1});
    CHECK(row_text(screen, rows[0]).find("[Copiar] [Guardar]╮") != std::string::npos);

    // Salir del botón (aunque sea al de guardar del mismo bloque) lo quita.
    view.show_copied(1);
    screen = draw_view(view, entries, 40, 10, 0, {1});
    REQUIRE(row_text(screen, rows[0]).find("[✓ Copiado]") != std::string::npos);
    view.set_hover(column_of(screen, rows[0], "[Guardar]"), rows[0]);
    screen = draw_view(view, entries, 40, 10, 0, {1});
    CHECK(row_text(screen, rows[0]).find("[✓ Copiado]") == std::string::npos);

    // Fuera del historial también.
    view.show_copied(2);
    screen = draw_view(view, entries, 40, 10, 0, {1});
    CHECK(row_text(screen, rows[1]).find("[✓ Copiado]") != std::string::npos);
    view.clear_hover();
    screen = draw_view(view, entries, 40, 10, 0, {1});
    CHECK(row_text(screen, rows[1]).find("[✓ Copiado]") == std::string::npos);

    // Nada de esto vuelve a dibujar las entradas.
    CHECK(view.draw_count() == draws);
}

TEST_CASE("botones: [✓] en la versión compacta", "[historial][botones]") {
    const std::vector<Entry> entries{make(EntryKind::Assistant, "```cpp\nint a;\n```")};
    HistoryView view;
    view.show_copied(1);
    const ftxui::Screen screen = draw_view(view, entries, 25, 5, 0, {1});
    CHECK(row_text(screen, 1).find("[✓] [G]╮") != std::string::npos);
}

TEST_CASE("historial: un mensaje del usuario de varias líneas conserva sus saltos",
          "[historial][varias]") {
    HistoryView view;
    const std::string text =
        draw(view, {make(EntryKind::User, "primera línea\nsegunda\n\n    if (x) {\n    }")}, 40);
    std::vector<std::string> rows;
    std::size_t start = 0;
    for (std::size_t end = text.find('\n'); end != std::string::npos;
         start = end + 1, end = text.find('\n', start)) {
        std::string row = text.substr(start, end - start);
        row.erase(row.find_last_not_of(' ') + 1); // Sin el relleno de la derecha.
        rows.push_back(row);
    }
    const auto at = std::find(rows.begin(), rows.end(), "primera línea");
    REQUIRE(at != rows.end());
    REQUIRE(rows.end() - at >= 5);
    CHECK(*(at + 1) == "segunda");
    CHECK(*(at + 2) == "");
    CHECK(*(at + 3) == "    if (x) {");
    CHECK(*(at + 4) == "    }");
}
