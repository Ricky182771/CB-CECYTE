// main.cpp — interfaz de terminal del chatbot (hito 3).
//
// Pantalla completa con FTXUI: historial arriba, caja de entrada abajo. La
// petición corre en un hilo de trabajo; todo lo que toca la pantalla o la
// Conversation se ejecuta en el hilo de la interfaz mediante App::Post.

#include "conversation.h"

#include "chatbot/chat_client.h"
#include "chatbot/config.h"
#include "chatbot/curl_transport.h"
#include "chatbot/error.h"
#include "chatbot/result.h"
#include "chatbot/types.h"

#include <ftxui/component/app.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>

#include <atomic>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using chatbot::cli::Entry;
using chatbot::cli::EntryKind;

/// Dibuja una entrada de la conversación con su etiqueta.
ftxui::Element render_entry(const Entry& entry) {
    switch (entry.kind) {
    case EntryKind::User:
        return ftxui::vbox({ftxui::text("Tú:") | ftxui::bold, ftxui::paragraph(entry.text)});
    case EntryKind::Assistant: {
        ftxui::Elements lines{ftxui::text("Asistente:") | ftxui::bold,
                              ftxui::paragraph(entry.text)};
        if (entry.incomplete) {
            lines.push_back(ftxui::text("(respuesta incompleta)") | ftxui::dim);
        }
        return ftxui::vbox(std::move(lines));
    }
    case EntryKind::Error:
        return ftxui::paragraph(entry.text) | ftxui::color(ftxui::Color::Red);
    }
    return ftxui::emptyElement();
}

} // namespace

int main() {
    // La configuración se carga antes de tocar la terminal: si falla, el
    // error sale por stderr sin abrir la pantalla completa.
    const chatbot::Result<chatbot::Config> config = chatbot::load_config();
    if (config.is_error()) {
        std::cerr << "[" << chatbot::error_kind_label(config.error().kind) << "] "
                  << config.error().message << '\n';
        return 1;
    }
    const std::string title = "Chatbot CECyTE — " + config.value().model;

    // El orden de declaración importa: se destruyen en orden inverso, y el
    // hilo de trabajo se une (join) antes de que se destruyan screen, client
    // o conversation. Las tareas que queden en la cola de screen apuntan a
    // conversation e input_text, así que esos dos se declaran antes.
    chatbot::cli::Conversation conversation;
    std::string input_text;
    chatbot::ChatClient client(config.value(), std::make_unique<chatbot::CurlTransport>());
    auto screen = ftxui::App::Fullscreen();
    std::atomic<bool> quitting{false};
    std::thread worker;

    // Lanza la petición en el hilo de trabajo. Solo se llama desde el hilo
    // de la interfaz. ChatClient no es seguro entre hilos: solo lo usa el
    // hilo de trabajo y nunca hay dos peticiones a la vez (submit devuelve
    // nullopt mientras hay una en curso).
    const auto send = [&] {
        std::optional<std::vector<chatbot::Message>> messages = conversation.submit(input_text);
        if (!messages.has_value()) {
            return; // Vacío u ocupado: Enter no hace nada.
        }
        input_text.clear();
        if (worker.joinable()) {
            worker.join(); // El anterior ya terminó: dejó de estar ocupado.
        }
        // El hilo de trabajo nunca toca conversation ni input_text: solo
        // manda tareas que los modifican en el hilo de la interfaz.
        worker = std::thread([&client, &screen, &quitting, &conversation, &input_text,
                              history = std::move(*messages)] {
            const chatbot::Result<void> result =
                client.complete_stream(history, [&](std::string_view delta) {
                    if (quitting.load()) {
                        return false; // Saliendo: cancela la petición.
                    }
                    // Copia: el string_view deja de ser válido al regresar.
                    screen.Post([&conversation, text = std::string{delta}] {
                        conversation.append_delta(text);
                    });
                    // Post de una tarea no redibuja por sí solo; un evento sí.
                    screen.PostEvent(ftxui::Event::Custom);
                    return true;
                });
            screen.Post([&conversation, &input_text, result] {
                const std::optional<std::string> restored =
                    result.is_ok() ? conversation.finish_success()
                                   : std::optional<std::string>{
                                         conversation.finish_error(result.error())};
                // Regresa el texto a la caja si está vacía: reenviar es solo Enter.
                if (restored.has_value() && input_text.empty()) {
                    input_text = *restored;
                }
            });
            screen.PostEvent(ftxui::Event::Custom);
        });
    };

    ftxui::InputOption input_option;
    input_option.multiline = false;
    input_option.on_enter = [&] {
        send();
        screen.PostEvent(ftxui::Event::Custom);
    };
    const ftxui::Component input =
        ftxui::Input(&input_text, "Escribe tu mensaje y presiona Enter", input_option);

    const ftxui::Component layout = ftxui::Renderer(input, [&] {
        ftxui::Elements entries;
        for (const Entry& entry : conversation.entries()) {
            if (!entries.empty()) {
                entries.push_back(ftxui::text(""));
            }
            entries.push_back(render_entry(entry));
        }
        // Lo más reciente siempre abajo: el marco se enfoca en el final.
        ftxui::Element history = ftxui::vbox(std::move(entries)) |
                                 ftxui::focusPositionRelative(0.f, 1.f) | ftxui::yframe |
                                 ftxui::flex;
        ftxui::Element status = conversation.busy() ? ftxui::text(" Pensando…") | ftxui::dim
                                                    : ftxui::emptyElement();
        return ftxui::vbox({
            ftxui::text(title) | ftxui::bold,
            std::move(history),
            ftxui::separator(),
            ftxui::hbox({ftxui::text("> "), input->Render() | ftxui::flex, std::move(status)}),
        });
    });

    // Ctrl+C se maneja aquí: con el manejo por defecto, FTXUI restaura la
    // terminal y relanza SIGINT dentro de Loop(), y el proceso muere sin
    // pasar por el join() de abajo. Así, Loop() regresa normalmente.
    screen.ForceHandleCtrlC(false);
    const ftxui::Component root = ftxui::CatchEvent(layout, [&](const ftxui::Event& event) {
        if (event == ftxui::Event::CtrlC) {
            screen.Exit();
            return true;
        }
        return false;
    });

    screen.Loop(root);

    // Fuera del loop: avisa al hilo de trabajo y espera a que termine antes
    // de que se destruya cualquier objeto que use. Nunca detach().
    // Limitación conocida (se resuelve en el hito 4): quitting solo se revisa
    // cuando llega un delta. Si el modelo no manda nada, o el cliente está en
    // una espera de reintento, la salida tarda hasta el siguiente chunk o el
    // timeout (más los reintentos, si aún no llegaba texto).
    quitting.store(true);
    if (worker.joinable()) {
        worker.join();
    }
    return 0;
}
