// chatbot-telegram: el bot de Telegram (long polling, sin webhook). Lee la
// configuración (--config o CHAT_TELEGRAM_CONFIG), revisa el token con
// getMe, pone el menú de comandos y atiende hasta SIGINT o SIGTERM.
// Solo Linux por ahora (AGENTS.md, sección "Addons").

#include "chatbot/telegram/api.h"
#include "chatbot/telegram/log.h"
#include "chatbot/telegram/outbox.h"
#include "chatbot/telegram/poller.h"
#include "chatbot/telegram/settings.h"
#include "chatbot/telegram/startup.h"

#include "chatbot/bot/bot.h"
#include "chatbot/bot/config.h"
#include "chatbot/bot/time_zone.h"

#include "chatbot/cancel_token.h"
#include "chatbot/chat_client.h"
#include "chatbot/curl_transport.h"
#include "chatbot/platform.h"
#include "chatbot/tavily_search.h"

#include <pthread.h>
#include <signal.h>

#include <atomic>
#include <cstdlib>
#include <filesystem>
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

using namespace chatbot;
using namespace chatbot::telegram;

constexpr std::string_view kUsage =
    "Uso: chatbot-telegram --config <archivo.json>\n"
    "     (o la variable CHAT_TELEGRAM_CONFIG con la ruta)\n"
    "\n"
    "Secretos: CHAT_TELEGRAM_TOKEN, CHAT_API_KEY y CHAT_SEARCH_API_KEY, o\n"
    "credentials.json (permisos 600) junto al archivo de configuración.\n"
    "Ver la sección \"Bot de Telegram\" del README.\n";

std::string_view real_env(std::string_view name) {
    const char* value = std::getenv(std::string{name}.c_str());
    return value != nullptr ? std::string_view{value} : std::string_view{};
}

int fail(std::string_view message) {
    std::cerr << "chatbot-telegram: " << message << '\n';
    return 1;
}

/// SIGINT y SIGTERM llegan solo a un hilo (sigwait), que cancela todo: el
/// long polling, los envíos y las respuestas en curso.
class SignalWatcher {
public:
    SignalWatcher(CancelToken& shutdown, bot::Log& log) : shutdown_(shutdown), log_(log) {
        sigemptyset(&signals_);
        sigaddset(&signals_, SIGINT);
        sigaddset(&signals_, SIGTERM);
        // Antes de crear cualquier hilo, para que todos las tengan bloqueadas.
        pthread_sigmask(SIG_BLOCK, &signals_, nullptr);
        thread_ = std::thread{[this] {
            int signal = 0;
            sigwait(&signals_, &signal);
            if (!finished_) {
                log_.write(bot::LogLevel::Info, std::string{"apagando senal="} +
                                                    (signal == SIGINT ? "SIGINT" : "SIGTERM"));
            }
            shutdown_.cancel();
            // En paralelo con el corte del long polling: así el apagado
            // completo tarda lo que tarde el más lento, no la suma.
            const std::lock_guard<std::mutex> lock(mutex_);
            if (bot_ != nullptr) {
                bot_->shutdown();
            }
            done_ = true;
        }};
    }

    ~SignalWatcher() {
        finished_ = true;
        if (thread_.joinable()) {
            if (!done_) {
                pthread_kill(thread_.native_handle(), SIGTERM); // Sigue en sigwait.
            }
            thread_.join();
        }
    }

    SignalWatcher(const SignalWatcher&) = delete;
    SignalWatcher& operator=(const SignalWatcher&) = delete;

    /// El bot que se apaga con la señal. watch(nullptr) espera a que termine
    /// un apagado que la señal ya empezó: después se puede destruir el bot.
    void watch(bot::Bot* bot) {
        const std::lock_guard<std::mutex> lock(mutex_);
        bot_ = bot;
    }

private:
    CancelToken& shutdown_;
    bot::Log& log_;
    sigset_t signals_{};
    std::atomic<bool> finished_{false};
    std::atomic<bool> done_{false};
    std::mutex mutex_;
    bot::Bot* bot_ = nullptr;
    std::thread thread_;
};

int run(const std::string& config_path) {
    StreamLog log{std::cerr};
    CancelToken shutdown;
    SignalWatcher signals{shutdown, log};

    Result<TelegramSettings> loaded = load_telegram_settings(config_path, real_env);
    if (loaded.is_error()) {
        return fail(loaded.error().message);
    }
    const TelegramSettings& settings = loaded.value();
    Result<bot::DayFunction> day = bot::day_in_zone(settings.time_zone);
    if (day.is_error()) {
        return fail(day.error().message);
    }

    std::vector<bot::UserId> invited;
    std::string invited_path;
    if (settings.data_dir.has_value()) {
        if (std::optional<std::string> error =
                create_private_directory(*settings.data_dir, FolderPrivacy::PrivateIfNew)) {
            return fail("No se pudo preparar data_dir (" + *settings.data_dir + "): " + *error);
        }
        invited_path = (std::filesystem::path{*settings.data_dir} / kInvitedFileName).string();
        Result<std::vector<bot::UserId>> saved = load_invited(invited_path);
        if (saved.is_error()) {
            return fail(saved.error().message);
        }
        invited = saved.value();
    }
    Result<bot::ValidBotConfig> config = bot::validate_bot_config(settings.bot);
    if (config.is_error()) {
        return fail(config_path + ": " + config.error().message);
    }

    BotApi api{settings.token, [] { return std::make_unique<CurlTransport>(); }};
    Result<BotIdentity> identity = connect_to_telegram(api, log, shutdown);
    if (identity.is_error()) {
        return identity.error().kind == ErrorKind::Cancelled ? 0 : fail(identity.error().message);
    }
    log.write(bot::LogLevel::Info, "telegram_conectado bot=@" + identity.value().username +
                                       " id=" + std::to_string(identity.value().id));
    const bool search = !settings.search_api_key.empty();
    if (std::optional<ChatError> error = api.set_my_commands(bot_commands(search), &shutdown)) {
        log.write(bot::LogLevel::Warning, "comandos_no_puestos http=" +
                                              std::to_string(error->http_status));
    }

    TelegramOutbox outbox{api, log, shutdown,
                          OutboxOptions{settings.bot.log_error_details}};
    outbox.start();
    bot::SystemClock clock;
    bot::BotDependencies dependencies{
        [model = settings.model] {
            return std::make_unique<ChatClient>(model, std::make_unique<CurlTransport>());
        },
        search ? bot::SearchFactory{[key = settings.search_api_key] {
            return std::unique_ptr<SearchProvider>{
                std::make_unique<TavilySearch>(key, std::make_unique<CurlTransport>())};
        }}
               : bot::SearchFactory{},
        outbox, clock, log};
    dependencies.day = day.value();
    dependencies.invited = invited;
    if (!invited_path.empty()) {
        dependencies.save_invited = [invited_path, &log](const std::vector<bot::UserId>& users) {
            if (std::optional<std::string> error = save_invited(invited_path, users)) {
                log.write(bot::LogLevel::Error, "invitados_error " + *error);
                return false;
            }
            return true;
        };
    }
    bot::Bot bot{config.value(), std::move(dependencies)};
    signals.watch(&bot);

    Poller poller{api,
                  PollerHandlers{[&bot](const bot::IncomingMessage& message) { bot.handle(message); },
                                 [&bot](bot::ChatId chat, bot::ReplyId reply) {
                                     (void)bot.stop(chat, reply);
                                 },
                                 [&bot] { bot.expire_idle(); }},
                  log, shutdown};
    const std::optional<ChatError> fatal = poller.run();
    signals.watch(nullptr);
    shutdown.cancel();
    bot.shutdown();
    outbox.stop();
    if (fatal.has_value()) {
        return fail("Telegram dejó de aceptar el token (HTTP " +
                    std::to_string(fatal->http_status) + "): revisa que no lo hayan cambiado en "
                                                         "@BotFather.");
    }
    log.write(bot::LogLevel::Info, "fin");
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    signal(SIGPIPE, SIG_IGN);
    std::optional<std::string> config_path;
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument{argv[i]};
        if (argument == "--help" || argument == "-h") {
            std::cout << kUsage;
            return 0;
        }
        if (argument == "--config" && i + 1 < argc) {
            config_path = argv[++i];
            continue;
        }
        std::cerr << "chatbot-telegram: argumento desconocido: " << argument << "\n\n" << kUsage;
        return 1;
    }
    if (!config_path.has_value()) {
        if (const std::string_view from_env = real_env(kConfigEnv); !from_env.empty()) {
            config_path = std::string{from_env};
        }
    }
    if (!config_path.has_value()) {
        std::cerr << "chatbot-telegram: falta el archivo de configuración.\n\n" << kUsage;
        return 1;
    }
    try {
        return run(*config_path);
    } catch (const std::exception& e) {
        return fail(std::string{"error inesperado: "} + e.what());
    }
}
