#include "conversation.h"
#include "history_view.h"

#include <catch2/catch_test_macros.hpp>

#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>

#include <chrono>
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

std::string draw(HistoryView& view, const std::vector<Entry>& entries, int width) {
    ftxui::Element element = view.render(entries, width, chatbot::cli::terminal_palette());
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
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
    // Con sanitizadores solo se mide.
#elif defined(__has_feature)
#if !__has_feature(address_sanitizer) && !__has_feature(thread_sanitizer)
    CHECK(steady < 50.0);
    CHECK(streaming < 50.0);
#endif
#else
    CHECK(steady < 50.0);
    CHECK(streaming < 50.0);
#endif
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
