// main.cpp — interfaz de terminal del chatbot.
//
// Pantalla completa con FTXUI: historial arriba, caja de entrada abajo. La
// petición corre en el hilo de trabajo de RequestRunner; todo lo que toca la
// pantalla o la Conversation se ejecuta en el hilo de la interfaz mediante
// App::Post.

#include "addon_launcher.h"
#include "addons_panel.h"
#include "child_process.h"
#include "block_actions.h"
#include "clipboard.h"
#include "code_blocks.h"
#include "command_menu.h"
#include "command_menu_view.h"
#include "command_parser.h"
#include "commands.h"
#include "conversation.h"
#include "conversation_list.h"
#include "conversation_store.h"
#include "downloads.h"
#include "glyphs.h"
#include "history_view.h"
#include "input_edit.h"
#include "key_log.h"
#include "models_loader.h"
#include "paste.h"
#include "provider_settings.h"
#include "request_runner.h"
#include "search_settings.h"
#include "settings_screen.h"
#include "sidebar.h"
#include "sidebar_view.h"
#include "status_line.h"
#include "terminal_reply.h"
#include "text_area.h"
#include "theme.h"
#include "title_bar.h"

#include "chatbot/addon_settings/paths.h"
#include "chatbot/addon_settings/registry.h"
#include "chatbot/addon_settings/secrets.h"
#include "chatbot/chat_client.h"
#include "chatbot/config.h"
#include "chatbot/credentials.h"
#include "chatbot/curl_transport.h"
#include "chatbot/error.h"
#include "chatbot/markdown/markdown.h"
#include "chatbot/platform.h"
#include "chatbot/result.h"
#include "chatbot/search_context.h"
#include "chatbot/system_prompt.h"
#include "chatbot/tavily_search.h"
#include "chatbot/types.h"
#include "chatbot/web_search.h"

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
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

/// Aviso mientras no hay una configuración completa (falta key o modelo).
constexpr const char* kSetupNotice = "Configura un proveedor para empezar.";

/// Líneas que mueve cada paso de la rueda del ratón.
constexpr int kWheelStep = 3;

/// Valor de una variable de entorno, o nullopt si no existe.
std::optional<std::string> env_value(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr ? std::optional<std::string>{value} : std::nullopt;
}

/// Como env_value, pero una variable vacía cuenta como no definida (igual
/// que en load_config).
std::optional<std::string> env_non_empty(const char* name) {
    std::optional<std::string> value = env_value(name);
    return value.has_value() && !value->empty() ? value : std::nullopt;
}

/// El entorno real del proceso, para load_search_api_key.
std::string_view process_env(std::string_view name) {
    const char* value = std::getenv(std::string{name}.c_str());
    return value != nullptr ? std::string_view{value} : std::string_view{};
}

/// El sistema real para F2 → Addons.
chatbot::cli::AddonsEnvironment addons_environment(chatbot::cli::ServiceWorker::Post post) {
    namespace settings = chatbot::addon_settings;
    const auto absolute = [](const char* name) {
        std::optional<std::string> value = env_value(name);
        return value.has_value() && value->starts_with('/') ? value : std::nullopt;
    };
    chatbot::cli::AddonsEnvironment environment;
    environment.post = std::move(post);
    environment.run = [](const std::vector<std::string>& argv,
                         const chatbot::cli::ProcessOptions& options) {
        return chatbot::cli::run_process(argv, options);
    };
    environment.locate = chatbot::cli::locate_addon;
    environment.portable = chatbot::process_portable_data_dir().has_value();
    environment.config_path = chatbot::default_config_path().value_or("");
    environment.credentials_path = chatbot::default_credentials_path().value_or("");
    const settings::AddonInfo& telegram = *settings::find_addon("telegram");
    environment.data_dir = settings::default_addon_data_dir(telegram.name);
    environment.unit_path = settings::default_unit_path(telegram.unit);
    environment.uid = chatbot::cli::current_user_id();
    environment.chatbot_path = chatbot::executable_path().value_or("");
    environment.xdg_config_home = absolute("XDG_CONFIG_HOME");
    environment.xdg_data_home = absolute("XDG_DATA_HOME");
    for (const std::string_view name :
         {settings::kTokenEnv, settings::kApiKeyEnv, settings::kSearchKeyEnv}) {
        if (env_non_empty(std::string{name}.c_str()).has_value()) {
            environment.env_defined.push_back(name);
        }
    }
    environment.home = env_value("HOME").value_or("");
    return environment;
}

/// Proveedor de /buscar con la key vigente (CHAT_SEARCH_API_KEY o
/// credentials.json): nullptr si no hay key. Si credentials.json no se puede
/// leer, también nullptr y el motivo en warning.
std::shared_ptr<chatbot::SearchProvider> make_search_provider(std::string& warning) {
    const std::optional<std::string> credentials_path = chatbot::default_credentials_path();
    if (!credentials_path.has_value() && env_non_empty("CHAT_SEARCH_API_KEY") == std::nullopt) {
        return nullptr;
    }
    const chatbot::Result<std::string> key =
        chatbot::load_search_api_key(process_env, credentials_path.value_or(""));
    if (key.is_error()) {
        warning = key.error().message;
        return nullptr;
    }
    if (key.value().empty()) {
        return nullptr;
    }
    return std::make_shared<chatbot::TavilySearch>(key.value(),
                                                   std::make_unique<chatbot::CurlTransport>());
}

/// Traduce una tecla de FTXUI a la de la barra de conversaciones.
chatbot::cli::ListKey list_key(const ftxui::Event& event) {
    using chatbot::cli::ListKey;
    const std::pair<const ftxui::Event*, ListKey> keys[] = {
        {&ftxui::Event::ArrowUp, ListKey::Up},     {&ftxui::Event::ArrowDown, ListKey::Down},
        {&ftxui::Event::PageUp, ListKey::PageUp},  {&ftxui::Event::PageDown, ListKey::PageDown},
        {&ftxui::Event::Home, ListKey::Home},      {&ftxui::Event::End, ListKey::End},
        {&ftxui::Event::Return, ListKey::Enter},   {&ftxui::Event::Escape, ListKey::Escape},
        {&ftxui::Event::Delete, ListKey::Delete},  {&ftxui::Event::Tab, ListKey::Tab},
    };
    for (const auto& [ftxui_event, key] : keys) {
        if (event == *ftxui_event) {
            return key;
        }
    }
    return ListKey::Other;
}

/// Traduce un evento de FTXUI para el acumulador del pegado entre corchetes.
chatbot::cli::PasteKey paste_key(const ftxui::Event& event) {
    using chatbot::cli::PasteKey;
    // Event::Custom es Special("\0"): también un NUL pegado, que se descarta.
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

/// Atajos de pegado de Windows: Ctrl+V (Ctrl+Shift+V llega igual, como el
/// byte 0x16) y Shift+Insert (ESC [ 2 ; 2 ~ = 1b 5b 32 3b 32 7e, que FTXUI
/// v7.0.3 entrega tal cual como Event::Special). Bytes confirmados en conhost
/// (Windows 11 LTSC 2024) con CHAT_DEBUG_KEYS.
bool is_paste_shortcut(const ftxui::Event& event) {
    return event == ftxui::Event::CtrlV ||
           event == ftxui::Event::Special(chatbot::cli::kShiftInsert);
}

/// Traduce un evento de FTXUI para el registro de teclas (key_log.h). La
/// vista apunta a event: debe vivir mientras se use.
chatbot::cli::KeyLogEvent key_log_event(ftxui::Event& event) { // mouse() no es const.
    using Kind = chatbot::cli::KeyLogEvent::Kind;
    chatbot::cli::KeyLogEvent out;
    if (event == ftxui::Event::Custom) {
        out.kind = Kind::Custom;
    } else if (event.is_mouse()) {
        static constexpr std::string_view kButtons[] = {
            "left", "middle", "right", "none", "wheel-up", "wheel-down", "wheel-left", "wheel-right"};
        static constexpr std::string_view kMotions[] = {"released", "pressed", "moved"};
        const ftxui::Mouse& mouse = event.mouse();
        out.kind = Kind::Mouse;
        out.button = mouse.button < std::size(kButtons) ? kButtons[mouse.button] : "?";
        out.motion = mouse.motion < std::size(kMotions) ? kMotions[mouse.motion] : "?";
        out.x = mouse.x;
        out.y = mouse.y;
    } else {
        out.kind = event.is_character() ? Kind::Character : Kind::Special;
        out.input = event.input();
    }
    return out;
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

    /// true si (x, y) cae en la zona visible del historial (último cuadro).
    [[nodiscard]] bool contains(int x, int y) const { return view_box_.Contain(x, y); }

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

int main(int argc, char** argv) {
    // Windows: la consola en UTF-8 mientras corre el programa, y al salir
    // (también por un error) vuelve a la página de códigos que tenía. Se
    // declara primero para que se destruya al último, después de FTXUI.
    const chatbot::ConsoleUtf8Scope console_utf8;
    // chatbot --help y chatbot <addon> [argumentos] (este se reemplaza por
    // el addon): antes de tocar la consola, la configuración o la pantalla.
    if (const std::optional<int> code =
            chatbot::cli::run_command_line(std::vector<std::string>(argv + 1, argv + argc))) {
        return *code;
    }
    // Windows: sin ENABLE_PROCESSED_INPUT, Ctrl+C llega como tecla y sale por
    // screen.Exit() (ver CatchEvent), en lugar de matar el proceso con
    // SIGINT dentro de Loop(). Se construye antes de Loop() y se destruye
    // después de que el runner cancela y hace join, así que Ctrl+C durante
    // ese join tampoco mata el proceso.
    const chatbot::ConsoleInputScope console_input;

    // Errores de arranque: por stderr, antes de abrir la pantalla completa.
    // Con doble clic en Windows, la ventana se cerraría al salir sin que
    // nadie leyera el error: ahí se espera un Enter.
    const auto fail = [](const chatbot::ChatError& error) {
        std::cerr << "[" << chatbot::error_kind_label(error.kind) << "] " << error.message
                  << '\n';
        if (chatbot::console_closes_on_exit()) {
            std::cerr << "\nPresiona Enter para cerrar esta ventana." << std::flush;
            // Un carácter, no una línea: sin ENABLE_PROCESSED_INPUT
            // (ConsoleInputScope) la consola no convierte Enter en "\r\n" y
            // getline esperaría un "\n" que nunca llega. Con ENABLE_LINE_INPUT
            // la lectura regresa al presionar Enter.
            (void)std::cin.get();
        }
        return 1;
    };

    // Modo portable (chatbot.portable junto al ejecutable): la configuración,
    // la key y las conversaciones van a datos/. Si no se puede escribir ahí,
    // se sale con el error; nunca se usa en silencio la carpeta del usuario.
    const std::optional<std::string>& portable_dir = chatbot::process_portable_data_dir();
    if (portable_dir.has_value()) {
        if (const std::optional<chatbot::ChatError> error =
                chatbot::prepare_portable_data_dir(*portable_dir)) {
            return fail(*error);
        }
    }

    // La configuración se carga antes de tocar la terminal: si falla, el
    // error sale por stderr sin abrir la pantalla completa. Si lo único que
    // falta es la key o el modelo, la app arranca con la pantalla de
    // configuración abierta; sin terminal (stdin o stdout redirigidos) no
    // hay pantalla que abrir y también sale con el error.
    chatbot::ConfigOptions load_options;
    load_options.allow_missing_key_and_model = true;
    const chatbot::Result<chatbot::Config> initial = chatbot::load_config(load_options);
    const std::optional<chatbot::ChatError> incomplete =
        initial.is_ok() ? chatbot::validate_config(initial.value()) : std::nullopt;
    const bool has_terminal = chatbot::stdio_is_terminal();
    if (initial.is_error() || (incomplete.has_value() && !has_terminal)) {
        return fail(initial.is_error() ? initial.error() : *incomplete);
    }
    chatbot::Config config = initial.value(); ///< La vigente; cambia al guardar la configuración.
    std::string model;                       ///< Modelo del cliente; vacío sin configuración.

    // Tema, fondo y símbolos de config.json ("appearance"). Un valor
    // desconocido usa el de por defecto y deja un aviso en la línea de
    // estado. Símbolos "Automático": Sencillos solo en la consola de Windows 10.
    const chatbot::cli::Symbols detected_symbols = chatbot::cli::detect_symbols(
        chatbot::current_os(), chatbot::windows_build(), env_value("WT_SESSION").has_value(),
        env_value("TERM_PROGRAM").has_value());
    std::string appearance_warning;
    chatbot::cli::Appearance appearance =
        chatbot::cli::resolve_appearance(chatbot::AppearanceValues{}, detected_symbols);
    if (const std::optional<std::string> path = chatbot::default_config_path()) {
        const chatbot::Result<chatbot::AppearanceValues> saved =
            chatbot::load_appearance_values(*path);
        if (saved.is_ok()) {
            appearance = chatbot::cli::resolve_appearance(saved.value(), detected_symbols,
                                                          &appearance_warning);
        } else {
            appearance_warning = saved.error().message;
        }
    }
    /// Con lo que se dibuja todo; cambia al guardar la apariencia.
    chatbot::cli::Palette palette{appearance};

    // Instrucciones de sistema de config.json ("system_prompt"). Una llave
    // que no es cadena usa las predeterminadas y deja un aviso.
    std::string startup_warning = appearance_warning;
    /// Las vigentes (vacías: sin mensaje de sistema); cambian al guardarlas.
    std::string system_prompt{chatbot::kDefaultSystemPrompt};
    if (const std::optional<std::string> path = chatbot::default_config_path()) {
        chatbot::ResolvedSystemPrompt resolved =
            chatbot::resolve_system_prompt(chatbot::load_system_prompt_value(*path));
        system_prompt = std::move(resolved.text);
        if (!resolved.warning.empty()) {
            startup_warning += (startup_warning.empty() ? "" : " ") + resolved.warning;
        }
    }

    // Búsqueda web (/buscar). Se comparte con el hilo de trabajo del runner,
    // así que se puede reemplazar al guardar la key aunque haya un hilo
    // terminado sin unir.
    std::string search_warning;
    std::shared_ptr<chatbot::SearchProvider> search_provider =
        make_search_provider(search_warning);
    if (!search_warning.empty()) {
        startup_warning += (startup_warning.empty() ? "" : " ") + search_warning;
    }
    bool searching = false; ///< /buscar en curso, antes de que llegue la respuesta.

    // El orden de declaración importa: se destruyen en orden inverso. El
    // runner se destruye antes que screen y client (cancela y hace join del
    // hilo, que usa ambos); igual el cargador de modelos, que une sus hilos.
    // Las tareas que queden en la cola de screen apuntan a conversation,
    // scroll, last_dropped y store: van antes que screen (la caja de entrada
    // la llevan por copia del shared_ptr).
    const chatbot::cli::ConversationStore store{chatbot::cli::default_data_dir().value_or("")};
    chatbot::cli::Conversation conversation{system_prompt};
    Scroll scroll;
    chatbot::cli::HistoryView history;
    chatbot::cli::CodeBlockIndex code_blocks; ///< Números de los bloques (/copiar, /guardar).
    std::size_t last_dropped = 0;
    chatbot::cli::Sidebar sidebar;
    sidebar.open(store.list(), conversation.id());
    std::string flash = startup_warning; ///< Aviso de la línea de estado hasta la siguiente tecla.
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
    ftxui::Box export_box;  ///< [Exportar] de la barra de título en el último cuadro.
    // Menú de comandos al escribir "/" en la caja, y sus filas en el último
    // cuadro (para el ratón).
    chatbot::cli::CommandMenu command_menu;
    ftxui::Box command_menu_box;
    std::vector<ftxui::Box> command_menu_rows;
    bool export_hover = false;
    bool dragging = false;  ///< Se arrastra el divisor: el ratón es de ResizableSplit.
    int right_tab = 0; ///< Lado derecho: 0 = conversación, 1 = configuración.
    // Sin configuración completa no hay cliente ni runner (la caja no envía).
    std::unique_ptr<chatbot::ChatClient> client;
    auto screen = ftxui::App::Fullscreen();
    const auto post = [&screen](std::function<void()> task) {
        screen.Post(std::move(task));
        // Post de una tarea no redibuja por sí solo; un evento sí.
        screen.PostEvent(ftxui::Event::Custom);
    };
    std::unique_ptr<chatbot::cli::RequestRunner> runner;
    chatbot::cli::ModelsLoader models_loader(
        [] { return std::make_unique<chatbot::CurlTransport>(); }, post);

    // (Re)construye el cliente y el runner con una configuración completa.
    // Solo sin respuesta en curso: el runner viejo se destruye (join) antes
    // que el cliente que usa.
    const auto connect = [&](const chatbot::Config& complete) {
        runner.reset();
        client = std::make_unique<chatbot::ChatClient>(complete,
                                                       std::make_unique<chatbot::CurlTransport>());
        runner = std::make_unique<chatbot::cli::RequestRunner>(*client,
                                                               complete.history_limit_bytes, post);
        model = complete.model;
    };
    if (!incomplete.has_value()) {
        connect(config);
    }
    const auto busy = [&] { return runner != nullptr && runner->busy(); };

    // La caja de entrada (text_area.h): el texto, el cursor y la vista son
    // suyos. Enter envía (on_enter, abajo); "\" + Enter y Alt+Enter hacen el
    // salto (root). Sin configuración, el placeholder lo dice.
    const auto input_area = chatbot::cli::make_text_area(
        {.palette = &palette, .min_rows = 1, .max_rows = chatbot::cli::kMaxInputLines,
         .enter = chatbot::cli::EnterKey::Submit, .on_enter = {}, .on_change = {},
         .placeholder = [&runner](int columns) {
             return runner != nullptr ? std::string{chatbot::cli::input_placeholder(columns)}
                                      : std::string{kSetupNotice} + " (F2)";
         }});
    const ftxui::Component input = input_area;

    /// Abrevia rutas con "~" (en Windows, vacía: las rutas van completas).
    const std::string home =
        chatbot::cli::display_home(chatbot::cli::environment_value, chatbot::current_os());
    // Carpeta de descargas (sin chatbot/) para /guardar y /exportar, o
    // nullopt con el motivo en la línea de estado.
    const auto download_base = [&]() -> std::optional<std::string> {
        std::optional<std::string> base =
            chatbot::cli::default_download_dir(chatbot::cli::environment_value);
        if (!base.has_value()) {
            flash = std::string{chatbot::cli::no_download_dir_message(chatbot::current_os())};
        }
        return base;
    };

    // /copiar, /guardar y /exportar: no van al modelo ni al historial. Los
    // errores de uso van en la línea de estado y el texto se queda en la caja;
    // el resultado, en el historial.
    const auto run_local_command = [&](const chatbot::cli::ParsedCommand& command) {
        using Type = chatbot::cli::ParsedCommand::Type;
        if (busy()) {
            flash = "Espera a que termine la respuesta";
            return;
        }
        // Muestra el resultado; la caja se vacía si salió bien (o siempre).
        const auto show = [&](const chatbot::cli::ActionResult& result, bool always_clear) {
            if (result.error) {
                conversation.add_error(result.message);
            } else {
                conversation.add_notice(result.message);
            }
            if (!result.error || always_clear) {
                input_area->set_text("");
            }
            scroll.to_bottom();
        };
        switch (command.type) {
        case Type::CopyUsage:
            flash = std::string{chatbot::cli::kCopyUsage};
            return;
        case Type::SaveUsage:
            flash = std::string{chatbot::cli::kSaveUsage};
            return;
        case Type::ExportUsage:
            flash = std::string{chatbot::cli::kExportUsage};
            return;
        case Type::Export: {
            if (!conversation.has_turns()) {
                flash = "Todavía no hay respuestas que exportar.";
                return;
            }
            if (const std::optional<std::string> base = download_base()) {
                show(chatbot::cli::export_conversation(conversation, std::time(nullptr), *base,
                                                       home),
                     false);
            }
            return;
        }
        default:
            break;
        }
        code_blocks.update(conversation.entries());
        const chatbot::cli::CodeBlockChoice choice =
            chatbot::cli::choose_code_block(code_blocks.blocks(), command.block);
        if (choice.block == nullptr) {
            flash = choice.problem;
            return;
        }
        if (command.type == Type::Save) {
            if (!command.file_name.empty()) {
                if (const std::optional<std::string> reason =
                        chatbot::cli::validate_file_name(command.file_name)) {
                    flash = *reason;
                    return;
                }
            }
            if (const std::optional<std::string> base = download_base()) {
                show(chatbot::cli::save_block(*choice.block, command.file_name, *base, home),
                     false);
            }
            return;
        }
        // Copy: la caja se vacía aunque no se haya podido copiar.
        show(chatbot::cli::copy_block(*choice.block, chatbot::cli::real_clipboard()), true);
    };

    // Envía el contenido de la caja. Solo se llama desde el hilo de la interfaz.
    const auto send = [&] {
        const auto command = chatbot::cli::parse_command(input_area->text());
        switch (command.type) {
        case chatbot::cli::ParsedCommand::Type::Copy:
        case chatbot::cli::ParsedCommand::Type::CopyUsage:
        case chatbot::cli::ParsedCommand::Type::Save:
        case chatbot::cli::ParsedCommand::Type::SaveUsage:
        case chatbot::cli::ParsedCommand::Type::Export:
        case chatbot::cli::ParsedCommand::Type::ExportUsage:
            // No necesitan modelo: funcionan aunque falte la configuración.
            run_local_command(command);
            return;
        default:
            break;
        }
        if (runner == nullptr) {
            flash = kSetupNotice; // Sin configuración, la caja no envía.
            return;
        }
        if (runner->busy()) {
            return; // Enter no hace nada mientras hay una respuesta en curso.
        }
        // /buscar: sin consulta o sin key, el texto se queda en la caja.
        if (command.type == chatbot::cli::ParsedCommand::Type::SearchEmpty) {
            flash = std::string{chatbot::cli::kSearchUsage};
            return;
        }
        const bool search = command.type == chatbot::cli::ParsedCommand::Type::Search;
        if (search && search_provider == nullptr) {
            flash = std::string{chatbot::cli::kMissingSearchKey};
            return;
        }
        auto messages = conversation.submit(input_area->text());
        if (!messages.has_value()) {
            return; // Caja vacía.
        }
        input_area->set_text("");
        scroll.to_bottom();
        const auto on_delta = [&conversation](std::string delta) {
            conversation.append_delta(delta);
        };
        const auto on_done =
            [&conversation, input_area, &last_dropped, &store, &sidebar, &model,
             &searching](chatbot::Result<chatbot::CompletionInfo> result, std::size_t dropped) {
                searching = false;
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
                if (restored.has_value() && input_area->text().empty()) {
                    input_area->set_text(*restored);
                }
            };
        if (!search) {
            runner->start(std::move(*messages), on_delta, on_done);
            return;
        }
        // La fecha se guarda con la búsqueda (AAAA-MM-DD) y el bloque la
        // lleva en español.
        const std::string date = chatbot::local_iso_date(std::time(nullptr));
        searching = true;
        runner->start_with_search(
            std::move(*messages), command.text, search_provider, chatbot::spanish_date(date),
            [&conversation, &searching, date](chatbot::cli::RequestRunner::SearchContext found) {
                searching = false;
                (void)conversation.attach_search(std::move(found.response), date,
                                                 std::move(found.block));
            },
            on_delta, on_done);
    };

    // La barra con lo que hay en el almacén y la conversación abierta.
    const auto refresh_sidebar = [&] { sidebar.refresh(store.list(), conversation.id()); };

    // Empieza una conversación nueva (la actual ya quedó guardada si tenía pares).
    const auto new_conversation = [&] {
        conversation = chatbot::cli::Conversation{system_prompt};
        last_dropped = 0;
        scroll.to_bottom();
        refresh_sidebar();
    };

    input_area->set_on_enter([&] {
        send();
        screen.PostEvent(ftxui::Event::Custom);
    });

    // Lo que el menú de comandos necesita saber para atenuar los que no se
    // pueden usar ahora.
    const auto command_context = [&] {
        code_blocks.update(conversation.entries());
        chatbot::cli::CommandContext context;
        context.configured = runner != nullptr;
        context.has_search_key = search_provider != nullptr;
        context.has_code_blocks = !code_blocks.blocks().empty();
        context.has_turns = conversation.has_turns();
        context.busy = busy();
        return context;
    };
    // Aplica lo que pidió el menú de comandos. true si el menú usó el evento.
    const auto apply_command_menu = [&](const chatbot::cli::CommandMenuAction& action) {
        using Type = chatbot::cli::CommandMenuAction::Type;
        switch (action.type) {
        case Type::Pass:
            return false;
        case Type::Handled:
            return true;
        case Type::Complete:
        case Type::Submit:
            input_area->set_text(action.text); // El cursor al final.
            if (action.type == Type::Submit) {
                send();
            }
            return true;
        }
        return false;
    };

    // Pantalla de configuración. Al cerrarse, vuelve la conversación.
    chatbot::cli::SettingsScreen settings(
        models_loader, palette,
        [&](const chatbot::cli::ProviderSettings& form) -> std::optional<std::string> {
            const std::optional<std::string> config_path = chatbot::default_config_path();
            const std::optional<std::string> credentials_path =
                chatbot::default_credentials_path();
            if (!config_path.has_value() || !credentials_path.has_value()) {
                return chatbot::missing_config_dir_message(chatbot::current_os());
            }
            // Primero la key: si falla, config.json no apunta a un proveedor sin key.
            if (const auto update = form.credential_update()) {
                chatbot::Result<chatbot::Credentials> saved =
                    chatbot::load_credentials(*credentials_path);
                if (saved.is_error()) {
                    return saved.error().message;
                }
                chatbot::Credentials credentials = saved.value();
                credentials.keys[update->first] = update->second;
                if (const auto error = chatbot::save_credentials(*credentials_path, credentials)) {
                    return error->message;
                }
            }
            if (const auto error = chatbot::save_config_file(*config_path, form.file_values())) {
                return error->message;
            }
            // Lo que quedó guardado más el entorno, validado como al arrancar.
            const chatbot::Result<chatbot::Config> fresh = chatbot::load_config();
            if (fresh.is_error()) {
                return fresh.error().message;
            }
            config = fresh.value();
            connect(config);
            conversation.add_notice("Ahora se usa " + model + " de " + form.provider_label() +
                                    ".");
            scroll.to_bottom();
            return std::nullopt;
        },
        [&](const chatbot::cli::Appearance& chosen) -> std::optional<std::string> {
            const std::optional<std::string> config_path = chatbot::default_config_path();
            if (!config_path.has_value()) {
                return chatbot::missing_config_dir_message(chatbot::current_os());
            }
            // Con NO_COLOR el tema y el fondo no se eligen: se quedan como
            // están en config.json (vacíos no se escriben).
            const bool no_color = chatbot::cli::no_color_enabled();
            const chatbot::AppearanceValues values{
                no_color ? std::string{} : std::string{chosen.theme->id},
                no_color ? std::string{}
                         : std::string{chatbot::cli::background_id(chosen.background)},
                std::string{chatbot::cli::symbol_mode_id(chosen.symbol_mode)}};
            if (const auto error = chatbot::save_appearance(*config_path, values)) {
                return error->message;
            }
            // Se aplica sin reiniciar: la caché del historial se invalida
            // sola (su llave incluye el tema, el fondo y los símbolos).
            appearance = chosen;
            appearance.symbols =
                chatbot::cli::resolve_symbols(chosen.symbol_mode, detected_symbols);
            palette = chatbot::cli::Palette{appearance};
            return std::nullopt;
        },
        [&](const std::optional<std::string>& stored) -> std::optional<std::string> {
            const std::optional<std::string> config_path = chatbot::default_config_path();
            if (!config_path.has_value()) {
                return chatbot::missing_config_dir_message(chatbot::current_os());
            }
            if (const auto error = chatbot::save_system_prompt(*config_path, stored)) {
                return error->message;
            }
            // Se aplica sin reiniciar y sin perder la conversación: valen
            // desde el siguiente mensaje (la pantalla no se abre con una
            // respuesta en curso).
            system_prompt = stored.value_or(std::string{chatbot::kDefaultSystemPrompt});
            (void)conversation.set_system_prompt(system_prompt);
            conversation.add_notice(system_prompt.empty()
                                        ? "Sin instrucciones del sistema desde el siguiente "
                                          "mensaje."
                                        : "Instrucciones del sistema actualizadas: valen desde "
                                          "el siguiente mensaje.");
            if (const std::optional<std::string> warning =
                    chatbot::system_prompt_limit_warning(system_prompt,
                                                              config.history_limit_bytes)) {
                conversation.add_notice(*warning);
            }
            scroll.to_bottom();
            return std::nullopt;
        },
        [&](const std::string& key) -> std::optional<std::string> {
            const std::optional<std::string> credentials_path =
                chatbot::default_credentials_path();
            if (!credentials_path.has_value()) {
                return chatbot::missing_config_dir_message(chatbot::current_os());
            }
            chatbot::Result<chatbot::Credentials> saved =
                chatbot::load_credentials(*credentials_path);
            if (saved.is_error()) {
                return saved.error().message;
            }
            chatbot::Credentials credentials = saved.value();
            credentials.keys[std::string{chatbot::kSearchCredentialsKey}] = key;
            if (const auto error = chatbot::save_credentials(*credentials_path, credentials)) {
                return error->message;
            }
            // Se aplica sin reiniciar (la pantalla no se abre con una
            // respuesta en curso).
            std::string warning;
            search_provider = make_search_provider(warning);
            if (!warning.empty()) {
                return warning;
            }
            conversation.add_notice("Key de búsqueda guardada: usa /buscar <consulta>.");
            scroll.to_bottom();
            return std::nullopt;
        },
        [&] {
            right_tab = 0;
            input->TakeFocus();
        });

    settings.set_portable_dir(portable_dir);
    settings.set_addons_environment(addons_environment(post));

    // El foco del lado derecho: la configuración si está abierta; si no, la caja.
    const auto focus_main = [&] {
        if (settings.is_open()) {
            settings.focus();
        } else {
            input->TakeFocus();
        }
    };

    // Abre la configuración (F2 o "⚙ Configuración"), con un aviso opcional.
    const auto open_settings = [&](std::string notice) {
        if (busy()) {
            flash = "Espera la respuesta o cancélala con Esc.";
            return;
        }
        if (settings.is_open()) {
            settings.focus();
            return;
        }
        const std::optional<std::string> config_path = chatbot::default_config_path();
        const std::optional<std::string> credentials_path = chatbot::default_credentials_path();
        if (!config_path.has_value() || !credentials_path.has_value()) {
            flash = chatbot::missing_config_dir_message(chatbot::current_os());
            return;
        }
        const chatbot::Result<chatbot::ConfigFileValues> values =
            chatbot::load_config_file_values(*config_path);
        if (values.is_error()) {
            flash = values.error().message;
            return;
        }
        const chatbot::Result<chatbot::Credentials> credentials =
            chatbot::load_credentials(*credentials_path);
        if (credentials.is_error()) {
            flash = credentials.error().message;
            return;
        }
        chatbot::cli::SettingsEnv env;
        env.base_url = env_non_empty("CHAT_BASE_URL");
        env.model = env_non_empty("CHAT_MODEL");
        env.api_key = env_non_empty("CHAT_API_KEY");
        const auto saved_search =
            credentials.value().keys.find(std::string{chatbot::kSearchCredentialsKey});
        chatbot::cli::SearchSettings search(
            saved_search != credentials.value().keys.end() ? saved_search->second : std::string{},
            env_non_empty("CHAT_SEARCH_API_KEY"));
        right_tab = 1;
        settings.open(chatbot::cli::ProviderSettings(values.value(), credentials.value(),
                                                     std::move(env)),
                      config, appearance, system_prompt, std::move(search), std::move(notice));
    };

    // Ejecuta lo que pidió la barra de conversaciones.
    const auto apply_sidebar_action = [&](const chatbot::cli::ListAction& action) {
        using Type = chatbot::cli::ListAction::Type;
        if (action.type == Type::None) {
            return;
        }
        if (action.type == Type::Close) {
            focus_main();
            return;
        }
        if (action.type == Type::Cancelled) {
            flash = std::string{chatbot::cli::ConversationList::kDeleteCancelled};
            return;
        }
        if (action.type == Type::Settings) {
            open_settings({});
            return;
        }
        if (busy()) {
            // Abrir, borrar o crear cambiaría la conversación en curso.
            flash = "Espera la respuesta o cancélala con Esc.";
            return;
        }
        if ((action.type == Type::New || action.type == Type::Open) && settings.is_open() &&
            !settings.request_close()) {
            // Cambios sin guardar: la configuración pregunta antes de cerrarse.
            settings.focus();
            return;
        }
        switch (action.type) {
        case Type::None:
        case Type::Close:
        case Type::Settings:
        case Type::Cancelled:
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
            conversation =
                chatbot::cli::Conversation::from_stored(*loaded.conversation, system_prompt);
            const std::string used = conversation.last_model();
            if (!used.empty() && !model.empty() && used != model) {
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
                                            ftxui::Terminal::Size().dimy, focused, palette) |
               ftxui::reflect(sidebar_box);
    });

    const ftxui::Component chat = ftxui::Renderer(input, [&] {
        // El historial ocupa el ancho que deja la barra (más su divisor y un
        // espacio de separación).
        const int width = ftxui::Terminal::Size().dimx - (sidebar_visible ? split_size + 2 : 0);
        code_blocks.update(conversation.entries());
        ftxui::Element body = scroll.apply(
            history.render(conversation.entries(), width, palette, code_blocks.first_numbers()));
        const ftxui::Decorator notice = palette.ink(&chatbot::cli::Theme::notice);

        // Los avisos usan lo que dejan los indicadores (reserved) y se
        // recortan con "…" (status_line.h).
        std::vector<chatbot::cli::StatusItem> status;
        if (sidebar_visible && sidebar_panel->Focused()) {
            if (const std::optional<std::string> question = sidebar.confirmation()) {
                status.push_back({*question, ftxui::bold});
            } else if (flash.empty() && !busy()) {
                // La ayuda solo si no hay otro aviso: juntos no caben.
                status.push_back({"Enter abre · Supr borra · Esc vuelve a la caja", notice});
            }
        }
        if (!flash.empty()) {
            status.push_back({flash, ftxui::bold});
        }
        if (busy()) {
            status.push_back({searching ? "Buscando en la web… (Esc para cancelar)"
                                        : "Pensando… (Esc para cancelar)",
                              ftxui::nothing, true});
        }
        if (scroll.has_more_below()) {
            // Con texto en la caja, End mueve el cursor: el aviso sugiere PgDn.
            status.push_back({input_area->text().empty() ? "↓ Hay más abajo (End)"
                                                 : "↓ Hay más abajo (PgDn)",
                              ftxui::bold, true});
        }
        if (last_dropped > 0) {
            status.push_back({"Se omitieron " + std::to_string(last_dropped) +
                                  " mensajes antiguos para no exceder el límite.",
                              notice});
        }

        // Menú de comandos: entre la línea de estado y la caja; empuja el
        // historial hacia arriba.
        command_menu.update(input_area->text(), input->Focused());
        ftxui::Element menu = ftxui::emptyElement();
        if (command_menu.is_open()) {
            menu = chatbot::cli::render_command_menu(
                       command_menu, chatbot::cli::command_availability(command_context()),
                       width, palette, &command_menu_rows) |
                   ftxui::reflect(command_menu_box);
        } else {
            command_menu_rows.clear();
            command_menu_box = ftxui::Box{};
        }
        const bool hide_status = command_menu.is_open() && status.empty();
        input_area->set_max_rows(chatbot::cli::input_max_rows(ftxui::Terminal::Size().dimy));
        const std::string title =
            "Chatbot CECyTE — " + (model.empty() ? std::string{"sin configurar"} : model) + " — " +
            (conversation.title().empty() ? std::string{"Nueva conversación"}
                                          : chatbot::md::sanitize(conversation.title()));
        ftxui::Element content = ftxui::vbox({
            chatbot::cli::title_bar(title, width, palette,
                                    {conversation.has_turns(), export_hover}, export_box),
            std::move(body),
            // Con el menú abierto y sin avisos, el renglón de estado y su
            // línea se ocultan: la línea del menú lo separa del historial.
            hide_status ? ftxui::emptyElement()
                        : ftxui::separator() | palette.ink(&chatbot::cli::Theme::border),
            hide_status ? ftxui::emptyElement() : chatbot::cli::status_line(status, width),
            std::move(menu),
            // El prompt en negritas marca la caja sin un bloque de color. La
            // caja mide su alto ella misma: crece con sus filas en pantalla
            // hasta su tope y después se desplaza con su barra.
            ftxui::hbox({ftxui::text("> ") | ftxui::bold |
                             palette.ink(&chatbot::cli::Theme::user_label),
                         input->Render()}),
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
    // A la derecha, la conversación o la configuración.
    const ftxui::Component settings_panel = ftxui::Renderer(settings.component(), [&] {
        ftxui::Element element = settings.component()->Render();
        // Un espacio entre el borde de la barra y la ventana, como en la conversación.
        return sidebar_visible ? ftxui::hbox({ftxui::text(" "), std::move(element) | ftxui::flex})
                               : element;
    });
    split_option.back = ftxui::Container::Tab({chat, settings_panel}, &right_tab);
    split_option.direction = ftxui::Direction::Left;
    split_option.main_size = &split_size;
    split_option.separator_func = [&] {
        return sidebar_visible ? chatbot::cli::sidebar_divider(palette) : ftxui::emptyElement();
    };
    split_option.min = &split_min;
    split_option.max = &split_max;
    const ftxui::Component split = ftxui::ResizableSplit(split_option);

    // Oculta la barra; si tenía el foco, pasa al lado derecho.
    const auto hide_sidebar = [&] {
        if (sidebar_panel->Focused()) {
            focus_main();
        }
        sidebar_visible = false;
        dragging = false;
    };

    // Pegado entre corchetes: acumula lo que llega entre 200~ y 201~ (ver root).
    chatbot::cli::BracketedPaste bracketed_paste;

    const ftxui::Component layout = ftxui::Renderer(split, [&] {
        // Con un pegado abierto, cada cuadro le dice a BracketedPaste que el
        // loop ya procesó lo que leyó: su salida de emergencia solo cuenta ese
        // tiempo, no lo que FTXUI tarda en parsear un lote grande. Y pide el
        // siguiente cuadro, para que los haya aunque la terminal no mande nada.
        if (bracketed_paste.active()) {
            bracketed_paste.frame();
            screen.RequestAnimationFrame();
        }
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
        // Fondo y texto del tema en toda la pantalla (Palette::base).
        return split->Render() | palette.base();
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
        if (mouse.motion == ftxui::Mouse::Moved) {
            if (!settings.is_open() && scroll.contains(mouse.x, mouse.y)) {
                history.set_hover(mouse.x, mouse.y);
            } else {
                history.clear_hover();
            }
            export_hover = export_box.Contain(mouse.x, mouse.y);
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
        if (settings.is_open()) {
            return false;
        }
        const bool click =
            mouse.button == ftxui::Mouse::Left && mouse.motion == ftxui::Mouse::Pressed;
        // Menú de comandos: hover mueve el cursor y clic completa (como Tab).
        // Nada del ratón sobre el menú llega al historial (tampoco la rueda).
        if (command_menu.is_open() && command_menu_box.Contain(mouse.x, mouse.y)) {
            for (std::size_t row = 0; row < command_menu_rows.size(); ++row) {
                if (!command_menu_rows[row].Contain(mouse.x, mouse.y)) {
                    continue;
                }
                if (mouse.motion == ftxui::Mouse::Moved) {
                    command_menu.select(row);
                } else if (click) {
                    (void)apply_command_menu(command_menu.choose(row));
                }
            }
            return true;
        }
        // Botones de los bloques y [Exportar] (sin respuestas no hace nada):
        // también con una respuesta en curso. El resultado va a la línea de
        // estado; no tocan el historial, el scroll ni la caja.
        if (const std::optional<chatbot::cli::ButtonHit> hit = history.hit_test(mouse.x, mouse.y);
            click && hit.has_value()) {
            code_blocks.update(conversation.entries());
            const chatbot::cli::ActionResult result = chatbot::cli::run_block_action(
                code_blocks.blocks(), hit->block, hit->action, chatbot::cli::real_clipboard(),
                chatbot::cli::default_download_dir(chatbot::cli::environment_value), home,
                palette.symbols());
            flash = result.message;
            if (!result.error && hit->action == chatbot::cli::BlockAction::Copy) {
                history.show_copied(hit->block);
            }
            return true;
        }
        if (click && export_box.Contain(mouse.x, mouse.y) && conversation.has_turns()) {
            if (const std::optional<std::string> base = download_base()) {
                flash = chatbot::cli::export_conversation(conversation, std::time(nullptr), *base,
                                                          home)
                            .message;
            }
            return true;
        }
        // La rueda sobre la caja la desplaza si su texto no cabe; si no,
        // mueve el historial.
        if ((mouse.button == ftxui::Mouse::WheelUp || mouse.button == ftxui::Mouse::WheelDown) &&
            input_area->scrollable() && input_area->contains(mouse.x, mouse.y)) {
            (void)input->OnEvent(event);
            return true;
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

    // Pegado entre corchetes (DECSET 2004): se activa con la primera tarea
    // del loop, cuando FTXUI ya instaló la terminal (en conhost, el modo VT
    // de salida lo activa FTXUI) y se desactiva antes de salir del loop.
    // Todo va por std::cout, el canal de FTXUI, entre cuadros. El acumulador
    // (bracketed_paste) se declara antes de layout, que le avisa de cada cuadro.
    bool paste_mode_lost = false; ///< POSIX: se suspendió con Ctrl+Z (se desactivó).
    const auto quit = [&] {
        (void)chatbot::cli::write_to_terminal(chatbot::cli::kBracketedPasteOff);
        screen.Exit();
    };

    // Aviso en la línea de estado o, con la configuración abierta, bajo su
    // formulario (la línea de estado no se ve).
    const auto notify = [&](std::string_view notice) {
        if (settings.is_open()) {
            settings.show_notice(std::string{notice});
        } else {
            flash = std::string{notice};
        }
    };
    // A dónde iría un pegado según el foco (paste.h).
    const auto current_paste_target = [&] {
        return chatbot::cli::paste_target(
            sidebar_visible && sidebar_panel->Focused(), settings.is_open(),
            settings.is_open() ? settings.paste_target() : chatbot::cli::PasteTarget::None);
    };
    // Inserta un pegado (entre corchetes o con un atajo) donde está el foco:
    // un solo Event::Character con todo el texto, que Input::HandleCharacter
    // inserta de una vez en el cursor. Así sirve en la caja y en todos los
    // campos de la configuración. Con la barra lateral, se ignora.
    // Devuelve true si insertó algo.
    const auto deliver_paste = [&](std::string_view raw, bool truncated) {
        const std::optional<chatbot::cli::SanitizedPaste> paste =
            chatbot::cli::prepare_paste(raw, current_paste_target());
        if (!paste.has_value()) {
            return false;
        }
        const ftxui::Event event = ftxui::Event::Character(paste->text);
        if (settings.is_open()) {
            (void)settings.component()->OnEvent(event);
        } else if (event == ftxui::Event::Return) {
            // Event compara solo el texto: un "\n" pegado solo sería Enter
            // para la caja y enviaría el mensaje.
            input_area->insert(paste->text);
        } else {
            (void)input->OnEvent(event);
        }
        if (truncated || paste->truncated) {
            notify(chatbot::cli::kPasteTruncated);
        }
        return true;
    };

    // Registro de teclas para diagnosticar (CHAT_DEBUG_KEYS, key_log.h).
    chatbot::cli::KeyLog key_log{chatbot::cli::environment_value("CHAT_DEBUG_KEYS")};
    const auto log_event = [&](ftxui::Event& event) {
        chatbot::cli::KeyLogState state;
        state.settings_open = settings.is_open();
        state.sidebar_focused = sidebar_visible && sidebar_panel->Focused();
        state.paste_target = current_paste_target();
        state.bracketed_paste_open = bracketed_paste.active();
        if (settings.is_open()) {
            state.focused_fields = settings.debug_focus();
        }
        key_log.write(chatbot::cli::format_key_event(
            chatbot::cli::key_log_time(std::chrono::system_clock::now()), key_log_event(event),
            state));
    };

    const ftxui::Component root = ftxui::CatchEvent(layout, [&](ftxui::Event event) {
        if (key_log.enabled()) {
            log_event(event);
        }
        if (paste_mode_lost) {
            // De vuelta de Ctrl+Z (fg): el shell pudo haberlo desactivado.
            (void)chatbot::cli::write_to_terminal(chatbot::cli::kBracketedPasteOn);
            paste_mode_lost = false;
        }
        // Una respuesta de la terminal que FTXUI no entendió (DCS, DA) no
        // es una tecla: no borra el aviso ni llega a los componentes.
        if (!event.is_character() && !event.is_mouse() &&
            chatbot::cli::is_terminal_reply(event.input())) {
            return true;
        }
        // Lo que llega entre las marcas del pegado es texto, aunque sea
        // Enter, Tab o Ctrl+C. Sin 201~ y tras más de kPasteIdleLimit con
        // cuadros y sin entrada, el pegado se entrega y este evento sigue
        // normal (así Ctrl+C siempre puede salir).
        const chatbot::cli::BracketedPaste::Step pasted = bracketed_paste.feed(
            paste_key(event), event.is_character() ? event.character() : std::string{});
        if (pasted.text.has_value()) {
            flash.clear(); // Como cualquier tecla.
            deliver_paste(*pasted.text, pasted.truncated);
        }
        if (pasted.consumed) {
            return true;
        }
        if (event == ftxui::Event::CtrlC) {
            quit();
            return true;
        }
        if (event == ftxui::Event::Custom) {
            return false; // Solo pide redibujar.
        }
        // Con una pregunta (s/n) abierta, todo evento es de ella (yes_no.h):
        // ni el ratón, ni F2, ni Ctrl+B/O/N la saltan.
        if (settings.is_open() && settings.asking()) {
            (void)settings.component()->OnEvent(event);
            return true;
        }
        if (sidebar_visible && sidebar_panel->Focused() && sidebar.confirmation().has_value()) {
            if (!event.is_mouse()) {
                flash.clear();
                apply_sidebar_action(sidebar.handle(
                    list_key(event), event.is_character() ? event.character() : std::string{},
                    sidebar_rows_visible()));
            }
            return true;
        }
        if (event.is_mouse()) {
            return handle_mouse(std::move(event));
        }
        flash.clear(); // El aviso dura hasta la siguiente tecla.
        if (event == ftxui::Event::CtrlZ && chatbot::current_os() == chatbot::Os::Posix) {
            // FTXUI suspende el proceso (SIGTSTP): el shell no debe recibir
            // las marcas. Al volver se activa otra vez (arriba).
            (void)chatbot::cli::write_to_terminal(chatbot::cli::kBracketedPasteOff);
            paste_mode_lost = true;
            return false;
        }
        if (chatbot::current_os() == chatbot::Os::Windows && is_paste_shortcut(event)) {
            // conhost no pega con la entrada en modo VT (la activa FTXUI):
            // la app lee el portapapeles. En Linux la terminal pega sola.
            const chatbot::cli::PasteTarget target = current_paste_target();
            if (target == chatbot::cli::PasteTarget::None) {
                // Una lista, un botón o una pregunta: aviso. La barra lateral: nada.
                if (const auto notice = chatbot::cli::paste_shortcut_notice(
                        sidebar_visible && sidebar_panel->Focused(), target)) {
                    notify(*notice);
                }
                key_log.write(chatbot::cli::key_log_time(std::chrono::system_clock::now()) +
                              " paste-shortcut ignored target=none");
                return true;
            }
            const std::optional<std::string> text = chatbot::cli::read_native_clipboard();
            const bool delivered = text.has_value() && deliver_paste(*text, false);
            if (!text.has_value()) {
                notify(chatbot::cli::kClipboardNoText);
            }
            key_log.write(chatbot::cli::format_paste_shortcut(
                chatbot::cli::key_log_time(std::chrono::system_clock::now()),
                text.has_value() ? std::optional<std::size_t>{text->size()} : std::nullopt,
                delivered));
            return true;
        }
        if (event == ftxui::Event::F2) {
            if (settings.is_open()) {
                (void)settings.request_close();
            } else {
                open_settings({});
            }
            return true;
        }
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
            if (key == chatbot::cli::ListKey::Delete && busy()) {
                flash = "Espera la respuesta o cancélala con Esc.";
                return true;
            }
            apply_sidebar_action(sidebar.handle(
                key, event.is_character() ? event.character() : std::string{},
                sidebar_rows_visible()));
            return true;
        }
        if (settings.is_open()) {
            // La configuración recibe todas las teclas: si alguna llegara al
            // contenedor del split, podría mover el foco a la barra.
            (void)settings.component()->OnEvent(event);
            return true;
        }
        // Menú de comandos abierto: ↑/↓, Tab, Enter y Esc son primero suyos
        // (el primer Esc lo cierra; el segundo ya cancela la respuesta).
        command_menu.update(input_area->text(), input->Focused());
        if (command_menu.is_open()) {
            using chatbot::cli::ListKey;
            const ListKey key = list_key(event);
            if ((key == ListKey::Up || key == ListKey::Down || key == ListKey::Tab ||
                 key == ListKey::Enter || key == ListKey::Escape) &&
                apply_command_menu(command_menu.handle(key))) {
                return true;
            }
        }
        // "\" + Enter: salto de línea en lugar de enviar.
        if (event == ftxui::Event::Return && input_area->backslash_newline()) {
            return true;
        }
        // Alt+Enter (donde llega: en Windows es pantalla completa).
        if (event == ftxui::Event::Special(chatbot::cli::kAltEnter) ||
            event == ftxui::Event::Special(chatbot::cli::kAltEnterLf)) {
            input_area->insert("\n");
            return true;
        }
        // ←/→, ↑/↓ y Tab son de la caja: si llegaran al contenedor del
        // split, moverían el foco a la barra.
        if (event == ftxui::Event::ArrowLeft || event == ftxui::Event::ArrowRight ||
            event == ftxui::Event::ArrowUp || event == ftxui::Event::ArrowDown ||
            event == ftxui::Event::Tab || event == ftxui::Event::TabReverse) {
            input->OnEvent(event);
            return true;
        }
        if (event == ftxui::Event::Escape) {
            if (runner != nullptr) {
                runner->cancel(); // Sin petición en curso no hace nada (no borra la caja).
            }
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
        if (event == ftxui::Event::Home && input_area->text().empty()) {
            scroll.to_top();
            return true;
        }
        if (event == ftxui::Event::End && input_area->text().empty()) {
            scroll.to_bottom();
            return true;
        }
        return false;
    });

    if (incomplete.has_value()) {
        // Falta la key o el modelo. Los avisos del tema y de las
        // instrucciones, si hay, van también.
        const std::string notice = startup_warning.empty()
                                       ? std::string{kSetupNotice}
                                       : std::string{kSetupNotice} + " " + startup_warning;
        open_settings(notice);
        flash = notice; // Si no se pudo abrir, al menos el aviso.
    } else {
        input->TakeFocus();
    }
    screen.Post([] { (void)chatbot::cli::write_to_terminal(chatbot::cli::kBracketedPasteOn); });
    screen.Loop(root);

    // Al regresar de Loop(), el runner se destruye antes que screen y client
    // (y el cargador de modelos une sus hilos):
    // cancela la petición en curso y hace join (alrededor de 1 s como máximo,
    // aunque el modelo no esté mandando nada o haya una espera de reintento).
    return 0;
}
