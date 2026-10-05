// md_preview: dibuja un archivo markdown como lo muestra el chatbot, para
// revisar el render sin la API.
// Uso: md_preview [--width N] [--theme ID] [--background theme|terminal] archivo.md
// Con colores si la salida es una terminal; si no, texto plano. Sin --theme,
// el tema por defecto del chatbot.

#include "markdown.h"
#include "markdown_view.h"
#include "theme.h"

#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/string.hpp>

#include <unistd.h>

#include <charconv>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

namespace {

constexpr int kMaxWidth = 1000;

void usage() {
    std::cerr << "Uso: md_preview [--width N] [--theme ID] [--background theme|terminal] "
                 "archivo.md\n";
}

/// Texto plano de la pantalla, sin estilos (como Screen::ToString).
std::string plain_text(const ftxui::Screen& screen) {
    std::string out;
    for (int y = 0; y < screen.dimy(); ++y) {
        std::string row;
        bool previous_wide = false;
        for (int x = 0; x < screen.dimx(); ++x) {
            const std::string& character = screen.CellAt(x, y).character;
            if (!previous_wide) {
                row += character.empty() ? std::string{" "} : character;
            }
            previous_wide = character.size() > 1 && ftxui::string_width(character) == 2;
        }
        row.erase(row.find_last_not_of(' ') + 1);
        out += row + "\n";
    }
    return out;
}

} // namespace

int main(int argc, char** argv) {
    int width = 80;
    std::string path;
    const chatbot::cli::Theme* theme = &chatbot::cli::default_theme();
    chatbot::cli::BackgroundMode background = chatbot::cli::BackgroundMode::FromTheme;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--width" && i + 1 < argc) {
            const std::string_view value = argv[++i];
            const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), width);
            if (error != std::errc{} || end != value.data() + value.size() || width < 1 ||
                width > kMaxWidth) {
                std::cerr << "md_preview: --width debe ser un número entre 1 y " << kMaxWidth
                          << ".\n";
                return 2;
            }
        } else if (arg == "--theme" && i + 1 < argc) {
            theme = chatbot::cli::find_theme(argv[++i]);
            if (theme == nullptr) {
                std::cerr << "md_preview: tema desconocido. Temas:";
                for (const chatbot::cli::Theme& known : chatbot::cli::themes()) {
                    std::cerr << " " << known.id;
                }
                std::cerr << ".\n";
                return 2;
            }
        } else if (arg == "--background" && i + 1 < argc) {
            const auto mode = chatbot::cli::parse_background(argv[++i]);
            if (!mode.has_value()) {
                std::cerr << "md_preview: --background debe ser theme o terminal.\n";
                return 2;
            }
            background = *mode;
        } else if (path.empty() && !arg.empty() && arg.front() != '-') {
            path = arg;
        } else {
            usage();
            return 2;
        }
    }
    if (path.empty()) {
        usage();
        return 2;
    }
    std::ifstream file{path, std::ios::binary};
    if (!file) {
        std::cerr << "md_preview: no se pudo abrir " << path << ".\n";
        return 1;
    }
    std::ostringstream content;
    content << file.rdbuf();

    const chatbot::cli::Palette palette(*theme, background);
    ftxui::Element element =
        chatbot::cli::md::render(chatbot::cli::md::parse(content.str()), width, palette) |
        palette.base();
    ftxui::Screen screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(width),
                                                 ftxui::Dimension::Fit(element, true));
    ftxui::Render(screen, element);
    if (isatty(STDOUT_FILENO) != 0) {
        std::cout << screen.ToString() << "\n";
    } else {
        std::cout << plain_text(screen);
    }
    return 0;
}
