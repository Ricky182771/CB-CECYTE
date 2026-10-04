// main.cpp — interfaz de terminal del chatbot.
//
// Pantalla completa con FTXUI: historial arriba, caja de entrada abajo. La
// petición corre en el hilo de trabajo de RequestRunner; todo lo que toca la
// pantalla o la Conversation se ejecuta en el hilo de la interfaz mediante
// App::Post.

#include "conversation.h"
#include "conversation_list.h"
#include "conversation_store.h"
#include "history_view.h"
#include "input_style.h"
#include "markdown.h"
#include "request_runner.h"
#include "sidebar.h"
#include "sidebar_view.h"

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
#include <ftxui/screen/terminal.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

/// Líneas que mueve cada paso de la rueda del ratón.
constexpr int kWheelStep = 3;

/// Valor de una variable de entorno, o nullopt si no existe.
std::optional<std::string> env_value(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr ? std::optional<std::string>{value} : std::nullopt;
}

/// Traduce una tecla de FTXUI a la de la barra de conversaciones.
chatbot::cli::ListKey list_key(const ftxui::Event& event) {
    using chatbot::cli::ListKey;
    const std::pair<const ftxui::Event*, ListKey> keys[] = {
        {&ftxui::Event::ArrowUp, ListKey::Up},     {&ftxui::Event::ArrowDown, ListKey::Down},
        {&ftxui::Event::PageUp, ListKey::PageUp},  {&ftxui::Event::PageDown, ListKey::PageDown},
        {&ftxui::Event::Home, ListKey::Home},      {&ftxui::Event::End, ListKey::End},
        {&ftxui::Event::Return, ListKey::Enter},   {&ftxui::Event::Escape, ListKey::Escape},
        {&ftxui::Event::Delete, ListKey::Delete},
    };
    for (const auto& [ftxui_event, key] : keys) {
        if (event == *ftxui_event) {
            return key;
        }
    }
    return ListKey::Other;
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
    const std::string model = config.value().model;

    // El orden de declaración importa: se destruyen en orden inverso. El
    // runner se destruye antes que screen y client (cancela y hace join del
    // hilo, que usa ambos). Las tareas que queden en la cola de screen apuntan
    // a conversation, input_text, scroll, last_dropped y store: van antes que
    // screen.
    const chatbot::cli::ConversationStore store{
        chatbot::cli::resolve_data_dir(env_value("CHAT_DATA_DIR"), env_value("XDG_DATA_HOME"),
                                       env_value("HOME"))
            .value_or("")};
    chatbot::cli::Conversation conversation;
    std::string input_text;
    Scroll scroll;
    chatbot::cli::HistoryView history;
    std::size_t last_dropped = 0;
    chatbot::cli::Sidebar sidebar;
    sidebar.open(store.list(), conversation.id());
    std::string flash; ///< Aviso de la línea de estado hasta la siguiente tecla.
    // Barra lateral: visible al arrancar si la terminal es ancha. Si el ancho
    // la ocultó, el ancho la vuelve a mostrar; si la ocultó Ctrl+B, no.
    bool last_wide = ftxui::Terminal::Size().dimx >= chatbot::cli::kSidebarMinTerminal;
    bool sidebar_visible = last_wide;
    bool hidden_by_width = !last_wide;
    // Ancho para ResizableSplit (main_size): la barra sin su borde derecho,
    // que es el divisor. sidebar_size guarda el ancho mientras está oculta.
    int sidebar_size = chatbot::cli::kSidebarWidth - 1;
    int split_size = sidebar_visible ? sidebar_size : 0;
    int split_min = chatbot::cli::kSidebarMinWidth - 1;
    int split_max = chatbot::cli::kSidebarMaxWidth - 1;
    ftxui::Box sidebar_box; ///< Zona de la barra en el último cuadro (para el ratón).
    bool dragging = false;  ///< Se arrastra el divisor: el ratón es de ResizableSplit.
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
            [&conversation, &input_text, &last_dropped, &store, &sidebar,
             &model](chatbot::Result<chatbot::CompletionInfo> result, std::size_t dropped) {
                last_dropped = dropped;
                const std::optional<std::string> restored =
                    result.is_ok()
                        ? conversation.finish_success(result.value().finish_reason, model)
                        : std::optional<std::string>{conversation.finish_error(result.error())};
                if (!restored.has_value()) {
                    // Par usuario/asistente completo: es lo único que se guarda.
                    chatbot::cli::save_conversation(conversation, store);
                    // La barra se refresca: la conversación sube a "Hoy".
                    sidebar.refresh(store.list(), conversation.id());
                }
                // Regresa el texto a la caja si está vacía: reenviar es solo Enter.
                if (restored.has_value() && input_text.empty()) {
                    input_text = *restored;
                }
            });
    };

    // La barra con lo que hay en el almacén y la conversación abierta.
    const auto refresh_sidebar = [&] { sidebar.refresh(store.list(), conversation.id()); };

    // Empieza una conversación nueva (la actual ya quedó guardada si tenía pares).
    const auto new_conversation = [&] {
        conversation = chatbot::cli::Conversation{};
        last_dropped = 0;
        scroll.to_bottom();
        refresh_sidebar();
    };

    ftxui::InputOption input_option;
    input_option.multiline = false;
    // Sin el fondo invertido del transform por defecto (ver input_style.h).
    input_option.transform = [](ftxui::InputState state) {
        return chatbot::cli::input_transform(std::move(state.element), state.hovered,
                                             state.focused, state.is_placeholder);
    };
    input_option.on_enter = [&] {
        send();
        screen.PostEvent(ftxui::Event::Custom);
    };
    const ftxui::Component input =
        ftxui::Input(&input_text, "Escribe tu mensaje y presiona Enter", input_option);

    // Ejecuta lo que pidió la barra de conversaciones.
    const auto apply_sidebar_action = [&](const chatbot::cli::ListAction& action) {
        using Type = chatbot::cli::ListAction::Type;
        if (action.type == Type::None) {
            return;
        }
        if (action.type == Type::Close) {
            input->TakeFocus();
            return;
        }
        if (runner.busy()) {
            // Abrir, borrar o crear cambiaría la conversación en curso.
            flash = "Espera la respuesta o cancélala con Esc.";
            return;
        }
        switch (action.type) {
        case Type::None:
        case Type::Close:
            break;
        case Type::New:
            new_conversation();
            input->TakeFocus();
            break;
        case Type::Open: {
            const chatbot::cli::LoadResult loaded = store.load(action.id);
            if (!loaded.conversation.has_value()) {
                flash = loaded.error;
                break;
            }
            conversation = chatbot::cli::Conversation::from_stored(*loaded.conversation);
            const std::string used = conversation.last_model();
            if (!used.empty() && used != model) {
                conversation.add_notice("Esta conversación usó " + used + "; se continúa con " +
                                        model + ".");
            }
            last_dropped = 0;
            scroll.to_bottom();
            refresh_sidebar();
            input->TakeFocus();
            break;
        }
        case Type::Delete:
            if (const std::optional<std::string> error = store.remove(action.id)) {
                flash = "No se pudo borrar la conversación: " + *error;
            } else if (action.id == conversation.id()) {
                new_conversation(); // Se borró la que estaba abierta.
            }
            refresh_sidebar();
            break;
        }
    };

    // Día de hoy en hora local, para los grupos por fecha.
    const auto today = [] {
        return chatbot::cli::local_day(std::time(nullptr)).value_or(chatbot::cli::CalendarDay{});
    };
    // Filas de la lista que caben en la barra (la terminal menos los bordes).
    const auto sidebar_rows_visible = [] {
        return static_cast<std::size_t>(
            chatbot::cli::sidebar_view_height(ftxui::Terminal::Size().dimy));
    };

    // La barra lateral. Es enfocable (TakeFocus) para que la caja de entrada
    // pierda el cursor mientras se navega la lista; el ratón sobre ella lo
    // maneja root, que no le pasa esos eventos (si no, tomaría el foco con
    // solo pasar el puntero).
    const ftxui::Component sidebar_panel = ftxui::Renderer([&](bool focused) {
        if (!sidebar_visible) {
            sidebar_box = ftxui::Box{};
            return ftxui::emptyElement();
        }
        const std::vector<chatbot::cli::SidebarRow> rows = sidebar.rows(today());
        sidebar.fit(rows, sidebar_rows_visible());
        return chatbot::cli::render_sidebar(sidebar, rows, split_size,
                                            ftxui::Terminal::Size().dimy, focused) |
               ftxui::reflect(sidebar_box);
    });

    const ftxui::Component chat = ftxui::Renderer(input, [&] {
        // El historial ocupa el ancho que deja la barra (más su divisor y un
        // espacio de separación).
        const int width = ftxui::Terminal::Size().dimx - (sidebar_visible ? split_size + 2 : 0);
        ftxui::Element body = scroll.apply(history.render(conversation.entries(), width));

        ftxui::Elements status;
        if (sidebar_visible && sidebar_panel->Focused()) {
            if (const std::optional<std::string> question = sidebar.confirmation()) {
                status.push_back(ftxui::text(*question) | ftxui::bold);
            } else if (flash.empty() && !runner.busy()) {
                // La ayuda solo si no hay otro aviso: juntos no caben.
                status.push_back(
                    ftxui::text("Enter abre · Supr borra · Esc vuelve a la caja") | ftxui::dim);
            }
        }
        if (!flash.empty()) {
            status.push_back(ftxui::text(flash) | ftxui::bold);
        }
        if (runner.busy()) {
            status.push_back(ftxui::text("Pensando… (Esc para cancelar)"));
        }
        if (scroll.has_more_below()) {
            // Con texto en la caja, End mueve el cursor: el aviso sugiere PgDn.
            status.push_back(ftxui::text(input_text.empty() ? "↓ Hay más abajo (End)"
                                                            : "↓ Hay más abajo (PgDn)") |
                             ftxui::bold);
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

        const std::string title =
            "Chatbot CECyTE — " + model + " — " +
            (conversation.title().empty() ? std::string{"Nueva conversación"}
                                          : chatbot::cli::md::sanitize(conversation.title()));
        ftxui::Element content = ftxui::vbox({
            // Si no cabe, se encoge el título y el aviso de teclas queda entero.
            ftxui::hbox({ftxui::text(title) | ftxui::bold | ftxui::flex_shrink, ftxui::filler(),
                         ftxui::text(" Ctrl+B barra · Ctrl+O conversaciones") | ftxui::dim}),
            std::move(body),
            ftxui::separator(),
            ftxui::hbox(std::move(status_line)),
            // El prompt en negritas marca la caja sin un bloque de color.
            ftxui::hbox({ftxui::text("> ") | ftxui::bold, input->Render() | ftxui::flex}),
        });
        if (sidebar_visible) {
            // Un espacio entre el borde de la barra y la conversación.
            content = ftxui::hbox({ftxui::text(" "), std::move(content) | ftxui::flex});
        }
        return content;
    });

    // Barra a la izquierda, conversación a la derecha. El divisor es el borde
    // derecho de la barra y se arrastra con el ratón.
    ftxui::ResizableSplitOption split_option;
    split_option.main = sidebar_panel;
    split_option.back = chat;
    split_option.direction = ftxui::Direction::Left;
    split_option.main_size = &split_size;
    split_option.separator_func = [&] {
        return sidebar_visible ? chatbot::cli::sidebar_divider() : ftxui::emptyElement();
    };
    split_option.min = &split_min;
    split_option.max = &split_max;
    const ftxui::Component split = ftxui::ResizableSplit(split_option);

    // Oculta la barra; si tenía el foco, pasa a la caja de entrada.
    const auto hide_sidebar = [&] {
        if (sidebar_panel->Focused()) {
            input->TakeFocus();
        }
        sidebar_visible = false;
        dragging = false;
    };

    const ftxui::Component layout = ftxui::Renderer(split, [&] {
        const int width = ftxui::Terminal::Size().dimx;
        // Al cruzar los 100 columnas: por debajo se oculta sola; por encima
        // reaparece solo si la había ocultado el ancho (no Ctrl+B).
        const bool wide = width >= chatbot::cli::kSidebarMinTerminal;
        if (wide != last_wide) {
            if (!wide && sidebar_visible) {
                hide_sidebar();
                hidden_by_width = true;
            } else if (wide && hidden_by_width) {
                sidebar_visible = true;
                hidden_by_width = false;
            }
            last_wide = wide;
        }
        // Límites del arrastre: 18 a 60 columnas o la mitad de la terminal
        // (con los dos bordes; main_size no cuenta el divisor).
        split_min = chatbot::cli::kSidebarMinWidth - 1;
        split_max = std::max(split_min,
                             std::min(chatbot::cli::kSidebarMaxWidth, width / 2) - 1);
        if (sidebar_visible) {
            sidebar_size = std::clamp(split_size > 0 ? split_size : sidebar_size, split_min,
                                      split_max);
            split_size = sidebar_size;
        } else {
            split_size = 0;
        }
        return split->Render();
    });

    // Ctrl+C se maneja aquí: con el manejo por defecto, FTXUI restaura la
    // terminal y relanza SIGINT dentro de Loop(), y el proceso muere sin
    // destruir el runner (que cancela y hace join). Así, Loop() regresa.
    screen.ForceHandleCtrlC(false);

    // Ratón: la barra (rueda y clic), el divisor (de ResizableSplit) y el
    // historial (rueda).
    const auto handle_mouse = [&](ftxui::Event event) { // mouse() no es const.
        const ftxui::Mouse& mouse = event.mouse();
        if (dragging) {
            if (mouse.motion == ftxui::Mouse::Released) {
                dragging = false;
            }
            return false; // ResizableSplit cambia el ancho.
        }
        if (!sidebar_visible) {
            sidebar_box = ftxui::Box{};
        }
        const bool on_divider = sidebar_visible && mouse.x == sidebar_box.x_max + 1 &&
                                mouse.y >= sidebar_box.y_min && mouse.y <= sidebar_box.y_max;
        if (on_divider && mouse.button == ftxui::Mouse::Left &&
            mouse.motion == ftxui::Mouse::Pressed) {
            dragging = true;
            return false;
        }
        if (sidebar_visible && sidebar_box.Contain(mouse.x, mouse.y)) {
            const std::vector<chatbot::cli::SidebarRow> rows = sidebar.rows(today());
            if (mouse.button == ftxui::Mouse::WheelUp) {
                sidebar.scroll(-kWheelStep, rows, sidebar_rows_visible());
            } else if (mouse.button == ftxui::Mouse::WheelDown) {
                sidebar.scroll(kWheelStep, rows, sidebar_rows_visible());
            } else if (mouse.button == ftxui::Mouse::Left &&
                       mouse.motion == ftxui::Mouse::Pressed) {
                // Línea 0: el borde de arriba; luego, las filas visibles.
                const int line = mouse.y - sidebar_box.y_min - 1;
                if (line >= 0 && static_cast<std::size_t>(line) < sidebar_rows_visible()) {
                    apply_sidebar_action(
                        sidebar.click(static_cast<std::size_t>(line), rows));
                }
            }
            return true; // Ningún evento del ratón sobre la barra cambia el foco.
        }
        if (mouse.button == ftxui::Mouse::WheelUp) {
            scroll.by(-kWheelStep);
            return true;
        }
        if (mouse.button == ftxui::Mouse::WheelDown) {
            scroll.by(kWheelStep);
            return true;
        }
        return false;
    };

    const ftxui::Component root = ftxui::CatchEvent(layout, [&](ftxui::Event event) {
        if (event == ftxui::Event::CtrlC) {
            screen.Exit();
            return true;
        }
        if (event == ftxui::Event::Custom) {
            return false; // Solo pide redibujar.
        }
        if (event.is_mouse()) {
            return handle_mouse(std::move(event));
        }
        flash.clear(); // El aviso dura hasta la siguiente tecla.
        if (event == ftxui::Event::CtrlB) {
            if (sidebar_visible) {
                hide_sidebar();
            } else {
                sidebar_visible = true;
            }
            hidden_by_width = false;
            return true;
        }
        if (event == ftxui::Event::CtrlO) {
            sidebar_visible = true;
            hidden_by_width = false;
            refresh_sidebar();
            sidebar_panel->TakeFocus();
            return true;
        }
        if (event == ftxui::Event::CtrlN) {
            apply_sidebar_action({chatbot::cli::ListAction::Type::New, {}});
            return true;
        }
        if (sidebar_visible && sidebar_panel->Focused()) {
            // La barra recibe todas las teclas; la caja de entrada, ninguna.
            const chatbot::cli::ListKey key = list_key(event);
            if (key == chatbot::cli::ListKey::Delete && runner.busy()) {
                flash = "Espera la respuesta o cancélala con Esc.";
                return true;
            }
            apply_sidebar_action(sidebar.handle(
                key, event.is_character() ? event.character() : std::string{},
                sidebar_rows_visible()));
            return true;
        }
        // ←/→ y Tab son de la caja: si llegaran al contenedor del split,
        // moverían el foco a la barra.
        if (event == ftxui::Event::ArrowLeft || event == ftxui::Event::ArrowRight ||
            event == ftxui::Event::Tab || event == ftxui::Event::TabReverse) {
            input->OnEvent(event);
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
        return false;
    });

    input->TakeFocus();
    screen.Loop(root);

    // Al regresar de Loop(), el runner se destruye antes que screen y client:
    // cancela la petición en curso y hace join (alrededor de 1 s como máximo,
    // aunque el modelo no esté mandando nada o haya una espera de reintento).
    return 0;
}
