// main.cpp — interfaz de terminal del chatbot.
//
// Pantalla completa con FTXUI: historial arriba, caja de entrada abajo. La
// petición corre en el hilo de trabajo de RequestRunner; todo lo que toca la
// pantalla o la Conversation se ejecuta en el hilo de la interfaz mediante
// App::Post.

#include "command_parser.h"
#include "conversation.h"
#include "conversation_list.h"
#include "conversation_store.h"
#include "history_view.h"
#include "input_style.h"
#include "markdown.h"
#include "models_loader.h"
#include "provider_settings.h"
#include "request_runner.h"
#include "search_context.h"
#include "search_settings.h"
#include "settings_screen.h"
#include "sidebar.h"
#include "sidebar_view.h"
#include "system_prompt.h"
#include "theme.h"
#include "title_bar.h"

#include "chatbot/chat_client.h"
#include "chatbot/config.h"
#include "chatbot/credentials.h"
#include "chatbot/curl_transport.h"
#include "chatbot/error.h"
#include "chatbot/result.h"
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
#include <cstddef>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <unistd.h>

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
    // error sale por stderr sin abrir la pantalla completa. Si lo único que
    // falta es la key o el modelo, la app arranca con la pantalla de
    // configuración abierta; sin terminal (stdin o stdout redirigidos) no
    // hay pantalla que abrir y también sale con el error.
    chatbot::ConfigOptions load_options;
    load_options.allow_missing_key_and_model = true;
    const chatbot::Result<chatbot::Config> initial = chatbot::load_config(load_options);
    const std::optional<chatbot::ChatError> incomplete =
        initial.is_ok() ? chatbot::validate_config(initial.value()) : std::nullopt;
    const bool has_terminal = ::isatty(STDIN_FILENO) == 1 && ::isatty(STDOUT_FILENO) == 1;
    if (initial.is_error() || (incomplete.has_value() && !has_terminal)) {
        const chatbot::ChatError& error = initial.is_error() ? initial.error() : *incomplete;
        std::cerr << "[" << chatbot::error_kind_label(error.kind) << "] " << error.message
                  << '\n';
        return 1;
    }
    chatbot::Config config = initial.value(); ///< La vigente; cambia al guardar la configuración.
    std::string model;                       ///< Modelo del cliente; vacío sin configuración.

    // Tema y fondo de config.json ("appearance"). Un valor desconocido usa
    // el de por defecto y deja un aviso en la línea de estado.
    std::string appearance_warning;
    chatbot::cli::Appearance appearance = chatbot::cli::resolve_appearance("", "");
    if (const std::optional<std::string> path = chatbot::default_config_path()) {
        const chatbot::Result<chatbot::AppearanceValues> saved =
            chatbot::load_appearance_values(*path);
        if (saved.is_ok()) {
            appearance = chatbot::cli::resolve_appearance(
                saved.value().theme, saved.value().background, &appearance_warning);
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
    std::string system_prompt{chatbot::cli::kDefaultSystemPrompt};
    if (const std::optional<std::string> path = chatbot::default_config_path()) {
        chatbot::cli::ResolvedSystemPrompt resolved =
            chatbot::cli::resolve_system_prompt(chatbot::load_system_prompt_value(*path));
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
    // input_text, scroll, last_dropped y store: van antes que screen.
    const chatbot::cli::ConversationStore store{
        chatbot::cli::resolve_data_dir(env_value("CHAT_DATA_DIR"), env_value("XDG_DATA_HOME"),
                                       env_value("HOME"))
            .value_or("")};
    chatbot::cli::Conversation conversation{system_prompt};
    std::string input_text;
    Scroll scroll;
    chatbot::cli::HistoryView history;
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

    // Envía el contenido de la caja. Solo se llama desde el hilo de la interfaz.
    const auto send = [&] {
        if (runner == nullptr) {
            flash = kSetupNotice; // Sin configuración, la caja no envía.
            return;
        }
        if (runner->busy()) {
            return; // Enter no hace nada mientras hay una respuesta en curso.
        }
        // /buscar: sin consulta o sin key, el texto se queda en la caja.
        const chatbot::cli::ParsedCommand command = chatbot::cli::parse_command(input_text);
        if (command.type == chatbot::cli::ParsedCommand::Type::SearchEmpty) {
            flash = "Uso: /buscar <consulta>";
            return;
        }
        const bool search = command.type == chatbot::cli::ParsedCommand::Type::Search;
        if (search && search_provider == nullptr) {
            flash = std::string{chatbot::cli::kMissingSearchKey};
            return;
        }
        std::optional<std::vector<chatbot::Message>> messages = conversation.submit(input_text);
        if (!messages.has_value()) {
            return; // Caja vacía.
        }
        input_text.clear();
        scroll.to_bottom();
        const auto on_delta = [&conversation](std::string delta) {
            conversation.append_delta(delta);
        };
        const auto on_done =
            [&conversation, &input_text, &last_dropped, &store, &sidebar, &model,
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
                if (restored.has_value() && input_text.empty()) {
                    input_text = *restored;
                }
            };
        if (!search) {
            runner->start(std::move(*messages), on_delta, on_done);
            return;
        }
        // La fecha se guarda con la búsqueda (AAAA-MM-DD) y el bloque la
        // lleva en español.
        const std::string date = chatbot::cli::local_iso_date(std::time(nullptr));
        searching = true;
        runner->start_with_search(
            std::move(*messages), command.text, search_provider, chatbot::cli::spanish_date(date),
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

    ftxui::InputOption input_option;
    input_option.multiline = false;
    // Sin el fondo invertido del transform por defecto (ver input_style.h).
    input_option.transform = [&palette](ftxui::InputState state) {
        return chatbot::cli::input_transform(std::move(state.element), state.hovered,
                                             state.focused, state.is_placeholder, palette);
    };
    input_option.on_enter = [&] {
        send();
        screen.PostEvent(ftxui::Event::Custom);
    };
    // Sin configuración, el placeholder lo dice.
    std::string placeholder;
    const ftxui::Component input = ftxui::Input(&input_text, &placeholder, input_option);

    // Pantalla de configuración. Al cerrarse, vuelve la conversación.
    chatbot::cli::SettingsScreen settings(
        models_loader, palette,
        [&](const chatbot::cli::ProviderSettings& form) -> std::optional<std::string> {
            const std::optional<std::string> config_path = chatbot::default_config_path();
            const std::optional<std::string> credentials_path =
                chatbot::default_credentials_path();
            if (!config_path.has_value() || !credentials_path.has_value()) {
                return "No se encontró la carpeta de configuración (define HOME o "
                       "XDG_CONFIG_HOME).";
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
                return "No se encontró la carpeta de configuración (define HOME o "
                       "XDG_CONFIG_HOME).";
            }
            const chatbot::AppearanceValues values{
                std::string{chosen.theme->id},
                std::string{chatbot::cli::background_id(chosen.background)}};
            if (const auto error = chatbot::save_appearance(*config_path, values)) {
                return error->message;
            }
            // Se aplica sin reiniciar: la caché del historial se invalida
            // sola (su llave incluye el tema y el fondo).
            appearance = chosen;
            palette = chatbot::cli::Palette{appearance};
            return std::nullopt;
        },
        [&](const std::optional<std::string>& stored) -> std::optional<std::string> {
            const std::optional<std::string> config_path = chatbot::default_config_path();
            if (!config_path.has_value()) {
                return "No se encontró la carpeta de configuración (define HOME o "
                       "XDG_CONFIG_HOME).";
            }
            if (const auto error = chatbot::save_system_prompt(*config_path, stored)) {
                return error->message;
            }
            // Se aplica sin reiniciar y sin perder la conversación: valen
            // desde el siguiente mensaje (la pantalla no se abre con una
            // respuesta en curso).
            system_prompt = stored.value_or(std::string{chatbot::cli::kDefaultSystemPrompt});
            (void)conversation.set_system_prompt(system_prompt);
            conversation.add_notice(system_prompt.empty()
                                        ? "Sin instrucciones del sistema desde el siguiente "
                                          "mensaje."
                                        : "Instrucciones del sistema actualizadas: valen desde "
                                          "el siguiente mensaje.");
            if (const std::optional<std::string> warning =
                    chatbot::cli::system_prompt_limit_warning(system_prompt,
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
                return "No se encontró la carpeta de configuración (define HOME o "
                       "XDG_CONFIG_HOME).";
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
            flash = "No se encontró la carpeta de configuración (define HOME o XDG_CONFIG_HOME).";
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
        ftxui::Element body =
            scroll.apply(history.render(conversation.entries(), width, palette));
        const ftxui::Decorator notice = palette.ink(&chatbot::cli::Theme::notice);

        ftxui::Elements status;
        if (sidebar_visible && sidebar_panel->Focused()) {
            if (const std::optional<std::string> question = sidebar.confirmation()) {
                status.push_back(ftxui::text(*question) | ftxui::bold);
            } else if (flash.empty() && !busy()) {
                // La ayuda solo si no hay otro aviso: juntos no caben.
                status.push_back(
                    ftxui::text("Enter abre · Supr borra · Esc vuelve a la caja") | notice);
            }
        }
        if (!flash.empty()) {
            status.push_back(ftxui::text(flash) | ftxui::bold);
        }
        if (busy()) {
            status.push_back(ftxui::text(searching ? "Buscando en la web… (Esc para cancelar)"
                                                   : "Pensando… (Esc para cancelar)"));
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
                             notice);
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

        placeholder = runner != nullptr ? "Escribe tu mensaje y presiona Enter"
                                        : std::string{kSetupNotice} + " (F2)";
        const std::string title =
            "Chatbot CECyTE — " + (model.empty() ? std::string{"sin configurar"} : model) + " — " +
            (conversation.title().empty() ? std::string{"Nueva conversación"}
                                          : chatbot::cli::md::sanitize(conversation.title()));
        ftxui::Element content = ftxui::vbox({
            chatbot::cli::title_bar(title, width, palette),
            std::move(body),
            ftxui::separator() | palette.ink(&chatbot::cli::Theme::border),
            ftxui::hbox(std::move(status_line)),
            // El prompt en negritas marca la caja sin un bloque de color.
            ftxui::hbox({ftxui::text("> ") | ftxui::bold |
                             palette.ink(&chatbot::cli::Theme::user_label),
                         input->Render() | ftxui::flex}),
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
        // ←/→ y Tab son de la caja: si llegaran al contenedor del split,
        // moverían el foco a la barra.
        if (event == ftxui::Event::ArrowLeft || event == ftxui::Event::ArrowRight ||
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
    screen.Loop(root);

    // Al regresar de Loop(), el runner se destruye antes que screen y client
    // (y el cargador de modelos une sus hilos):
    // cancela la petición en curso y hace join (alrededor de 1 s como máximo,
    // aunque el modelo no esté mandando nada o haya una espera de reintento).
    return 0;
}
