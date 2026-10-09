#include "status_line.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/string.hpp>

#include <string>
#include <vector>

namespace {

using chatbot::cli::StatusItem;

/// La línea dibujada en una pantalla de width x 1, sin espacios al final.
std::string draw(const std::vector<StatusItem>& items, int width) {
    ftxui::Screen screen(width, 1);
    ftxui::Render(screen, chatbot::cli::status_line(items, width));
    std::string text;
    for (int x = 0; x < width; ++x) {
        const std::string& character = screen.CellAt(x, 0).character;
        text += character.empty() ? std::string{" "} : character;
    }
    text.erase(text.find_last_not_of(' ') + 1);
    return text;
}

const std::string kThinking = "Pensando… (Esc para cancelar)";
const std::string kIncomplete = "⚠ #1 incompleto (aún no termina) · copiado con wl-copy";

} // namespace

TEST_CASE("estado: ⚠ mide una columna", "[estado]") { CHECK(ftxui::string_width("⚠") == 1); }

TEST_CASE("estado: con una respuesta en curso, el indicador completo y el aviso recortado",
          "[estado]") {
    const int width = GENERATE(40, 80);
    INFO(width << " columnas");
    const std::vector<StatusItem> items{{kIncomplete, ftxui::bold}, {kThinking, {}, true}};
    const std::string row = draw(items, width);
    CHECK(row.ends_with("   " + kThinking));
    CHECK(ftxui::string_width(row) <= width);
    CHECK(ftxui::string_width(row) >= width - 1); // Sin el espacio antes del "…".
    const std::string notice = row.substr(0, row.size() - kThinking.size() - 3);
    CHECK(notice.ends_with("…"));
    CHECK(kIncomplete.starts_with(notice.substr(0, notice.size() - std::string{"…"}.size())));
    if (width == 80) {
        CHECK(notice == "⚠ #1 incompleto (aún no termina) · copiado con…");
    } else {
        CHECK(notice == "⚠ #1 in…");
    }
}

TEST_CASE("estado: si todo cabe, nada se recorta y el orden se conserva", "[estado]") {
    const std::vector<StatusItem> items{{"#1 copiado con wl-copy", ftxui::bold},
                                        {kThinking, {}, true},
                                        {"↓ Hay más abajo (End)", ftxui::bold, true}};
    CHECK(draw(items, 100) == "#1 copiado con wl-copy   " + kThinking + "   ↓ Hay más abajo (End)");
}

TEST_CASE("estado: el primer indicador nunca se quita; los siguientes, si no caben",
          "[estado]") {
    const std::vector<StatusItem> items{
        {"aviso", ftxui::bold}, {kThinking, {}, true}, {"↓ Hay más abajo (End)", {}, true}};
    // 29 + 3 + 21 = 53 > 40: el segundo indicador se quita y el aviso usa lo que sobra.
    CHECK(draw(items, 40) == "aviso   " + kThinking);
    CHECK(draw(items, 34) == "a…   " + kThinking);
    CHECK(draw(items, 33) == kThinking); // Un "…" solo no se muestra.
    CHECK(draw(items, 20).starts_with("Pensando… (Esc")); // Más angosto que el indicador.
}

TEST_CASE("estado: después de un aviso recortado no se muestran los demás", "[estado]") {
    const std::vector<StatusItem> items{{std::string(30, 'a'), {}},
                                        {"Se omitieron 4 mensajes antiguos.", {}}};
    CHECK(draw(items, 20) == std::string(19, 'a') + "…");
    CHECK(draw(items, 80) == std::string(30, 'a') + "   Se omitieron 4 mensajes antiguos.");
}

TEST_CASE("estado: sin partes mide una fila", "[estado]") {
    ftxui::Element element = chatbot::cli::status_line({}, 40);
    element->ComputeRequirement();
    CHECK(element->requirement().min_y == 1);
}
