// chatbot-rpc: la app por JSON-RPC 2.0 sobre stdin/stdout, una línea por
// mensaje (rpc/PROTOCOL.md). Aquí solo se arranca y se conectan los hilos y
// stdio; lo que se responde lo decide el Dispatcher.
//
// Hilos: el lector (stdin → líneas → cola), los de trabajo de Session (su
// post mete la tarea en la misma cola) y el principal, que saca de la cola,
// despacha y es el único que escribe en stdout.

#include "debug_log.h"
#include "dispatcher.h"
#include "line_reader.h"
#include "protocol.h"
#include "protocol_stdio.h"

#include "app_startup.h"
#include "clipboard.h"
#include "conversation_store.h"
#include "downloads.h"
#include "session.h"

#include "chatbot/config.h"
#include "chatbot/curl_transport.h"
#include "chatbot/error.h"
#include "chatbot/platform.h"

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using chatbot::rpc::Dispatcher;

/// Si salir se atora (un join que no termina), se sale de todos modos.
constexpr std::chrono::seconds kExitWatchdog{3};

/// Lo que llega al hilo principal.
struct Event {
    enum class Kind { Line, Oversized, Task, Eof };
    Kind kind = Kind::Eof;
    std::string line;           ///< Line.
    std::function<void()> task; ///< Task.
};

class EventQueue {
public:
    void push(Event event) {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            events_.push_back(std::move(event));
        }
        ready_.notify_one();
    }

    /// El siguiente evento, o nullopt si llegó deadline sin ninguno.
    std::optional<Event> pop(std::optional<Dispatcher::TimePoint> deadline) {
        std::unique_lock<std::mutex> lock(mutex_);
        const auto any = [this] { return !events_.empty(); };
        if (deadline.has_value()) {
            if (!ready_.wait_until(lock, *deadline, any)) {
                return std::nullopt;
            }
        } else {
            ready_.wait(lock, any);
        }
        Event event = std::move(events_.front());
        events_.pop_front();
        return event;
    }

private:
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<Event> events_;
};

Event to_event(chatbot::rpc::LineReader::Item item) {
    if (item.kind == chatbot::rpc::LineReader::Item::Kind::Oversized) {
        return Event{Event::Kind::Oversized, {}, {}};
    }
    return Event{Event::Kind::Line, std::move(item.text), {}};
}

/// Hilo lector: stdin en bloques (no getline, por el tope de la línea) →
/// líneas → cola; al final, Eof. Se queda bloqueado en read hasta que llega
/// algo, así que nunca se une: el proceso termina con std::_Exit.
void read_input(EventQueue& queue) {
    chatbot::rpc::LineReader reader;
    std::array<char, 64 * 1024> buffer{};
    while (true) {
        const long got = chatbot::rpc::read_stdin(buffer.data(), buffer.size());
        if (got <= 0) {
            break;
        }
        for (chatbot::rpc::LineReader::Item& item :
             reader.feed({buffer.data(), static_cast<std::size_t>(got)})) {
            queue.push(to_event(std::move(item)));
        }
    }
    if (std::optional<chatbot::rpc::LineReader::Item> last = reader.finish()) {
        queue.push(to_event(std::move(*last)));
    }
    queue.push(Event{Event::Kind::Eof, {}, {}});
}

/// Cierra la sesión (cancela y hace join) y sale con code, con un reloj de
/// seguridad por si el cierre se atora. std::_Exit: el hilo lector sigue
/// bloqueado en read y no se puede unir; todo lo escrito ya salió (write sin búfer).
/// Antes de salir restaura las páginas de códigos de la consola (_Exit no
/// corre destructores).
[[noreturn]] void quit(Dispatcher& dispatcher, int code,
                       const chatbot::ConsoleCodePages& console) {
    std::thread([code, console] {
        std::this_thread::sleep_for(kExitWatchdog);
        chatbot::restore_console_code_pages(console);
        std::_Exit(code);
    }).detach();
    dispatcher.on_eof();
    chatbot::restore_console_code_pages(console);
    std::_Exit(code);
}

/// Las CHAT_* que fijan algo de la configuración, por nombre (app.status).
std::vector<std::string> env_locks() {
    std::vector<std::string> defined;
    for (const char* name :
         {"CHAT_API_KEY", "CHAT_BASE_URL", "CHAT_MODEL", "CHAT_TIMEOUT", "CHAT_HISTORY_LIMIT",
          "CHAT_SEARCH_API_KEY", "CHAT_DATA_DIR", "CHAT_DOWNLOAD_DIR"}) {
        if (!chatbot::cli::process_env(name).empty()) {
            defined.emplace_back(name);
        }
    }
    return defined;
}

} // namespace

int main() {
    const int protocol = chatbot::rpc::take_protocol_stdout();
    if (protocol < 0) {
        std::cerr << "chatbot-rpc: no se pudo preparar stdout para el protocolo\n";
        return 1;
    }
    // Sin pipes (doble clic o desde una terminal) nadie le va a hablar: se
    // avisa una vez y sigue igual. La consola de Windows, en UTF-8 para los
    // acentos; en POSIX no hace nada.
    chatbot::ConsoleCodePages console{};
    if (chatbot::stdin_is_terminal()) {
        console = chatbot::set_console_utf8();
        std::cerr << "chatbot-rpc es para otras interfaces (la gráfica); para platicar, abre "
                     "chatbot.\nEscribe mensajes JSON-RPC, uno por línea, o cierra esta "
                     "ventana.\n";
    }
    chatbot::rpc::DebugLog log{std::string{chatbot::cli::process_env("CHAT_DEBUG_RPC")}};

    // El mismo arranque que la TUI (app_startup.h), pero un error no sale
    // del proceso: initialize lo responde (StartupFailed).
    const chatbot::Result<chatbot::Config> config = chatbot::cli::load_startup_config();
    chatbot::rpc::ServerInfo info;
    info.version = CHATBOT_VERSION;
    info.platform = chatbot::current_os() == chatbot::Os::Windows ? "windows" : "linux";
    info.portable = chatbot::process_portable_data_dir().has_value();
    info.config_path = chatbot::default_config_path();
    info.conversations_path = chatbot::cli::default_data_dir();
    if (const std::optional<std::string> base =
            chatbot::cli::default_download_dir(chatbot::cli::environment_value)) {
        info.downloads_path = (std::filesystem::path{*base} / "chatbot").string();
    }
    info.env_locks = env_locks();
    if (config.is_error()) {
        info.startup_error = "[" + std::string{chatbot::error_kind_label(config.error().kind)} +
                             "] " + config.error().message;
    }

    EventQueue queue;
    std::thread(read_input, std::ref(queue)).detach();

    bool write_failed = false;
    const auto write = [&](const chatbot::rpc::Json& message) {
        if (!write_failed) {
            log.outgoing(message);
            write_failed = !chatbot::rpc::write_all(protocol, chatbot::rpc::to_line(message));
        }
    };
    const auto post = [&queue](chatbot::cli::RequestRunner::Task task) {
        queue.push(Event{Event::Kind::Task, {}, std::move(task)});
    };
    const auto make_session = [&config](chatbot::cli::Session::Post session_post,
                                        chatbot::cli::Session::OnFinished on_finished) {
        return std::make_unique<chatbot::cli::Session>(chatbot::cli::app_session_environment(),
                                                       config.value(), std::move(session_post),
                                                       std::move(on_finished));
    };
    Dispatcher dispatcher(
        std::move(info), make_session, [] { return std::make_unique<chatbot::CurlTransport>(); },
        post, write, [] { return std::chrono::steady_clock::now(); });

    while (true) {
        std::optional<Event> event = queue.pop(dispatcher.next_deadline());
        bool eof = false;
        if (event.has_value()) {
            switch (event->kind) {
            case Event::Kind::Line:
                log.incoming(event->line);
                dispatcher.on_line(event->line);
                break;
            case Event::Kind::Oversized:
                log.note("línea de más de 1 MiB descartada");
                dispatcher.on_oversized_line();
                break;
            case Event::Kind::Task:
                event->task();
                dispatcher.after_task();
                break;
            case Event::Kind::Eof:
                eof = true;
                break;
            }
        }
        dispatcher.flush_due();
        // Sin cliente (EOF o no se pudo escribir): se cancela todo y sale con 0.
        if (eof || write_failed) {
            quit(dispatcher, 0, console);
        }
        if (const std::optional<int> code = dispatcher.exit_code()) {
            quit(dispatcher, *code, console);
        }
    }
}
