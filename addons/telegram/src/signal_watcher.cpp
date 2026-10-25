#include "signal_watcher.h"

#include <pthread.h>

#include <string>
#include <utility>

namespace chatbot::telegram {

SignalWatcher::SignalWatcher(CancelToken& shutdown, bot::Log& log)
    : shutdown_(shutdown), log_(log) {
    sigemptyset(&signals_);
    sigaddset(&signals_, SIGINT);
    sigaddset(&signals_, SIGTERM);
    sigaddset(&signals_, SIGHUP);
    pthread_sigmask(SIG_BLOCK, &signals_, nullptr);
    reload_thread_ = std::thread{[this] { reload_loop(); }};
    signal_thread_ = std::thread{[this] { wait_signals(); }};
}

SignalWatcher::~SignalWatcher() {
    finished_ = true;
    if (!done_) {
        send(SIGTERM); // Sigue en sigwait.
    }
    signal_thread_.join();
    reload_thread_.join();
}

void SignalWatcher::send(int signal) {
    pthread_kill(signal_thread_.native_handle(), signal);
}

void SignalWatcher::wait_signals() {
    int signal = 0;
    while (sigwait(&signals_, &signal) == 0) {
        if (finished_ || disabled_) {
            break;
        }
        if (signal != SIGHUP) {
            log_.write(bot::LogLevel::Info, std::string{"apagando senal="} +
                                                (signal == SIGINT ? "SIGINT" : "SIGTERM"));
            break;
        }
        request_reload();
    }
    // También corta una recarga a la mitad (setMyCommands).
    shutdown_.cancel();
    {
        const std::lock_guard<std::mutex> lock(wake_mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    // En paralelo con el corte del long polling: así el apagado completo
    // tarda lo que tarde el más lento, no la suma.
    {
        const std::lock_guard<std::mutex> lock(bot_mutex_);
        if (bot_ != nullptr) {
            bot_->shutdown();
        }
    }
    done_ = true;
}

void SignalWatcher::request_reload() {
    {
        const std::lock_guard<std::mutex> lock(wake_mutex_);
        requested_ = true;
    }
    wake_.notify_all();
}

void SignalWatcher::reload_loop() {
    while (true) {
        {
            std::unique_lock<std::mutex> lock(wake_mutex_);
            wake_.wait(lock, [this] { return stopping_ || requested_; });
            if (stopping_) {
                return;
            }
            requested_ = false;
        }
        const std::lock_guard<std::mutex> lock(reload_mutex_);
        if (!reload_) {
            // Arrancando (quizá esperando la red): se aplica en cuanto el
            // bot esté listo (watch).
            log_.write(bot::LogLevel::Info, "recarga_pendiente");
            pending_reload_ = true;
        } else if (!reload_()) {
            disabled_ = true;
            send(SIGHUP); // El hilo de señales apaga.
        }
    }
}

void SignalWatcher::watch(bot::Bot* bot, Reload reload) {
    const std::lock_guard<std::mutex> reload_lock(reload_mutex_);
    const std::lock_guard<std::mutex> bot_lock(bot_mutex_);
    bot_ = bot;
    reload_ = std::move(reload);
    if (reload_ && pending_reload_) {
        pending_reload_ = false;
        request_reload();
    }
}

} // namespace chatbot::telegram
