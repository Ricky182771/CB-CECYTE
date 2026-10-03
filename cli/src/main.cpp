// main.cpp — interfaz de terminal del chatbot.
//
// Pantalla completa con FTXUI: historial arriba, caja de entrada abajo. La
// petición corre en el hilo de trabajo de RequestRunner; todo lo que toca la
// pantalla o la Conversation se ejecuta en el hilo de la interfaz mediante
// App::Post.

#include "conversation.h"
#include "request_runner.h"

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
#include <ftxui/component/mouse.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/box.hpp>
#include <ftxui/screen/color.hpp>

#include <algorithm>
#include <cstddef>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using chatbot::cli::Entry;
using chatbot::cli::EntryKind;

/// Líneas que mueve cada paso de la rueda del ratón.
constexpr int kWheelStep = 3;

/// Dibuja una entrada de la conversación con su etiqueta.
ftxui::Element render_entry(const Entry& entry) {
    switch (entry.kind) {
    case EntryKind::User:
        return ftxui::vbox({ftxui::text("Tú:") | ftxui::bold, ftxui::paragraph(entry.text)});
    case EntryKind::Assistant: {
        ftxui::Elements lines{ftxui::text("Asistente:") | ftxui::bold,
                              ftxui::paragraph(entry.text)};
        if (entry.cancelled) {
            lines.push_back(ftxui::text("(cancelada)") | ftxui::dim);
        } else if (entry.incomplete) {
            lines.push_back(ftxui::text("(respuesta incompleta)") | ftxui::dim);
        } else if (!entry.note.empty()) {
            lines.push_back(ftxui::text(entry.note) | ftxui::dim);
        }
        return ftxui::vbox(std::move(lines));
    }
    case EntryKind::Error:
        return ftxui::paragraph(entry.text) | ftxui::color(ftxui::Color::Red);
    case EntryKind::Notice:
        return ftxui::paragraph(entry.text) | ftxui::dim;
    }
    return ftxui::emptyElement();
}

/// Igual que ftxui::reflect, pero guarda la caja completa que recibe el
/// elemento sin recortarla a la zona visible. Dentro de un frame, eso es el
/// alto total del contenido, que hace falta para limitar el scroll.
class MeasureBox : public ftxui::Node {
public:
    MeasureBox(ftxui::Element child, ftxui::Box& box)
        : ftxui::Node(ftxui::Elements{std::move(child)}), box_(box) {}

    void ComputeRequirement() override {
        ftxui::Node::ComputeRequirement();
        requirement_ = children_[0]->requirement();
    }

    void SetBox(ftxui::Box box) override {
        box_ = box;
        ftxui::Node::SetBox(box);
        children_[0]->SetBox(box);
    }

private:
    ftxui::Box& box_;
};

/// Posición del historial en pantalla. La vista sigue el final mientras el
/// usuario no suba; si sube, se queda donde está aunque llegue texto nuevo.
/// Se usa solo desde el hilo de la interfaz.
class Scroll {
public:
    /// Decora el historial: frame vertical posicionado según el estado.
    ftxui::Element apply(ftxui::Element content) {
        ftxui::Element measured = std::make_shared<MeasureBox>(std::move(content), content_box_);
        // Frame::SetBox centra el foco: con el foco en top + (alto - 1) / 2 la
        // primera línea visible es exactamente top (FTXUI v7.0.3, frame.cpp).
        ftxui::Element focused =
            follow_ ? measured | ftxui::focusPositionRelative(0.f, 1.f)
                    : measured | ftxui::focusPosition(0, top_ + (view_height() - 1) / 2);
        return focused | ftxui::yframe | ftxui::reflect(view_box_) | ftxui::flex;
    }

    void by(int lines) {
        const int base = follow_ ? max_top() : top_;
        top_ = std::clamp(base + lines, 0, max_top());
        follow_ = top_ >= max_top();
    }
    void page_up() { by(-page()); }
    void page_down() { by(page()); }
    void to_top() {
        top_ = 0;
        follow_ = max_top() == 0;
    }
    void to_bottom() { follow_ = true; }

    /// true si el usuario subió y hay contenido debajo de la vista.
    [[nodiscard]] bool has_more_below() const { return !follow_; }

private:
    [[nodiscard]] int view_height() const {
        return std::max(1, view_box_.y_max - view_box_.y_min + 1);
    }
    /// Una pantalla de líneas, dejando una de contexto.
    [[nodiscard]] int page() const { return std::max(1, view_height() - 1); }
    /// Mayor primera línea posible, con la misma cuenta que Frame::SetBox.
    [[nodiscard]] int max_top() const {
        const int internal = content_box_.y_max - content_box_.y_min;
        const int external = view_box_.y_max - view_box_.y_min;
        return std::max(0, internal - external - 1);
    }

    bool follow_ = true;
    int top_ = 0;
    ftxui::Box view_box_;    ///< Zona visible del historial (último render).
    ftxui::Box content_box_; ///< Historial completo (último render).
};

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

    // El orden de declaración importa: se destruyen en orden inverso. El
    // runner se destruye antes que screen y client (cancela y hace join del
    // hilo, que usa ambos). Las tareas que queden en la cola de screen apuntan
    // a conversation, input_text, scroll y last_dropped: van antes que screen.
    chatbot::cli::Conversation conversation;
    std::string input_text;
    Scroll scroll;
    std::size_t last_dropped = 0;
    chatbot::ChatClient client(config.value(), std::make_unique<chatbot::CurlTransport>());
    auto screen = ftxui::App::Fullscreen();
    chatbot::cli::RequestRunner runner(
        client, config.value().history_limit_bytes,
        [&screen](chatbot::cli::RequestRunner::Task task) {
            screen.Post(std::move(task));
            // Post de una tarea no redibuja por sí solo; un evento sí.
            screen.PostEvent(ftxui::Event::Custom);
        });

    // Envía el contenido de la caja. Solo se llama desde el hilo de la interfaz.
    const auto send = [&] {
        if (runner.busy()) {
            return; // Enter no hace nada mientras hay una respuesta en curso.
        }
        std::optional<std::vector<chatbot::Message>> messages = conversation.submit(input_text);
        if (!messages.has_value()) {
            return; // Caja vacía.
        }
        input_text.clear();
        scroll.to_bottom();
        runner.start(
            std::move(*messages),
            [&conversation](std::string delta) { conversation.append_delta(delta); },
            [&conversation, &input_text,
             &last_dropped](chatbot::Result<chatbot::CompletionInfo> result, std::size_t dropped) {
                last_dropped = dropped;
                const std::optional<std::string> restored =
                    result.is_ok() ? conversation.finish_success(result.value().finish_reason)
                                   : std::optional<std::string>{
                                         conversation.finish_error(result.error())};
                // Regresa el texto a la caja si está vacía: reenviar es solo Enter.
                if (restored.has_value() && input_text.empty()) {
                    input_text = *restored;
                }
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

        ftxui::Elements status;
        if (runner.busy()) {
            status.push_back(ftxui::text("Pensando… (Esc para cancelar)"));
        }
        if (scroll.has_more_below()) {
            status.push_back(ftxui::text("↓ Hay más abajo (End)") | ftxui::bold);
        }
        if (last_dropped > 0) {
            status.push_back(ftxui::text("Se omitieron " + std::to_string(last_dropped) +
                                         " mensajes antiguos para no exceder el límite.") |
                             ftxui::dim);
        }
        ftxui::Elements status_line;
        for (ftxui::Element& item : status) {
            if (!status_line.empty()) {
                status_line.push_back(ftxui::text("   "));
            }
            status_line.push_back(std::move(item));
        }
        if (status_line.empty()) {
            // Alto fijo: sin esto, un hbox vacío mide 0 líneas y la pantalla salta.
            status_line.push_back(ftxui::text(""));
        }

        return ftxui::vbox({
            ftxui::text(title) | ftxui::bold,
            scroll.apply(ftxui::vbox(std::move(entries))),
            ftxui::separator(),
            ftxui::hbox(std::move(status_line)),
            ftxui::hbox({ftxui::text("> "), input->Render() | ftxui::flex}),
        });
    });

    // Ctrl+C se maneja aquí: con el manejo por defecto, FTXUI restaura la
    // terminal y relanza SIGINT dentro de Loop(), y el proceso muere sin
    // destruir el runner (que cancela y hace join). Así, Loop() regresa.
    screen.ForceHandleCtrlC(false);
    const ftxui::Component root = ftxui::CatchEvent(layout, [&](ftxui::Event event) {
        if (event == ftxui::Event::CtrlC) {
            screen.Exit();
            return true;
        }
        if (event == ftxui::Event::Escape) {
            runner.cancel(); // Sin petición en curso no hace nada (no borra la caja).
            return true;
        }
        if (event == ftxui::Event::PageUp) {
            scroll.page_up();
            return true;
        }
        if (event == ftxui::Event::PageDown) {
            scroll.page_down();
            return true;
        }
        // Home/End: con texto en la caja mueven el cursor (los maneja la caja);
        // con la caja vacía mueven el historial.
        if (event == ftxui::Event::Home && input_text.empty()) {
            scroll.to_top();
            return true;
        }
        if (event == ftxui::Event::End && input_text.empty()) {
            scroll.to_bottom();
            return true;
        }
        if (event.is_mouse()) {
            if (event.mouse().button == ftxui::Mouse::WheelUp) {
                scroll.by(-kWheelStep);
                return true;
            }
            if (event.mouse().button == ftxui::Mouse::WheelDown) {
                scroll.by(kWheelStep);
                return true;
            }
        }
        return false;
    });

    screen.Loop(root);

    // Al regresar de Loop(), el runner se destruye antes que screen y client:
    // cancela la petición en curso y hace join (alrededor de 1 s como máximo,
    // aunque el modelo no esté mandando nada o haya una espera de reintento).
    return 0;
}
