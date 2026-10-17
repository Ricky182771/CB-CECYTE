// Programa mínimo para test_paste_pty.cpp (solo POSIX): un App de FTXUI a
// pantalla completa que pasa los eventos por BracketedPaste, como main.cpp
// (también le avisa de cada cuadro mientras hay un pegado abierto), y al
// terminar un pegado entre corchetes, con 201~ o por la salida de emergencia,
// escribe el texto en el archivo que recibe y sale. No abre la configuración
// ni la red: solo mide la entrada.
//
//   chatbot_paste_probe <archivo de resultado>
//
// Dibuja "paste-probe listo" desde el primer cuadro: para entonces FTXUI ya
// puso la terminal en modo crudo y la prueba puede escribir.

#include "paste.h"

#include <ftxui/component/app.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include <cstddef>
#include <fstream>
#include <iostream>
#include <string>

namespace {

/// Como paste_key de main.cpp.
chatbot::cli::PasteKey paste_key(const ftxui::Event& event) {
    using chatbot::cli::PasteKey;
    if (event.is_mouse() || event == ftxui::Event::Custom) {
        return PasteKey::Passthrough;
    }
    if (event == ftxui::Event::Special(chatbot::cli::kPasteStartMark)) {
        return PasteKey::Start;
    }
    if (event == ftxui::Event::Special(chatbot::cli::kPasteEndMark)) {
        return PasteKey::End;
    }
    if (event.is_character()) {
        return PasteKey::Character;
    }
    if (event == ftxui::Event::Return) {
        return PasteKey::Return;
    }
    if (event == ftxui::Event::Tab) {
        return PasteKey::Tab;
    }
    return PasteKey::Other;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "uso: chatbot_paste_probe <archivo de resultado>\n";
        return 2;
    }
    const std::string result_path = argv[1];

    auto screen = ftxui::App::Fullscreen();
    chatbot::cli::BracketedPaste paste;
    std::size_t events = 0;
    bool written = false;

    const ftxui::Component view = ftxui::Renderer([&] {
        // Como el renderer de layout en main.cpp.
        if (paste.active()) {
            paste.frame();
            screen.RequestAnimationFrame();
        }
        return ftxui::vbox({
            ftxui::text("paste-probe listo"),
            ftxui::text("eventos: " + std::to_string(events)),
        });
    });
    const ftxui::Component root = ftxui::CatchEvent(view, [&](ftxui::Event event) {
        ++events;
        const chatbot::cli::BracketedPaste::Step step =
            paste.feed(paste_key(event), event.is_character() ? event.character() : std::string{});
        if (step.text.has_value()) {
            std::ofstream out(result_path, std::ios::binary);
            out << *step.text;
            written = static_cast<bool>(out.flush());
            screen.Exit();
        }
        return true;
    });
    screen.Loop(root);
    return written ? 0 : 1;
}
