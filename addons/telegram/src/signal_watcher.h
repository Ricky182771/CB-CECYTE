#ifndef CHATBOT_TELEGRAM_SIGNAL_WATCHER_H
#define CHATBOT_TELEGRAM_SIGNAL_WATCHER_H

#include "chatbot/bot/bot.h"
#include "chatbot/bot/types.h"

#include "chatbot/cancel_token.h"

#include <signal.h>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace chatbot::telegram {

/// SIGINT, SIGTERM y SIGHUP llegan solo a un hilo (sigwait). Las dos
/// primeras cancelan todo: el long polling, los envíos, las respuestas en
/// curso y una recarga a la mitad. SIGHUP recarga la configuración en otro
/// hilo, para que el de señales siga atendiendo SIGTERM mientras la recarga
/// espera a Telegram; si la recarga pide apagar (enabled: false), apaga
/// igual que SIGTERM. Interno de chatbot-telegram (está aquí para probarlo).
class SignalWatcher {
public:
    /// false: hay que apagar el bot.
    using Reload = std::function<bool()>;

    /// Bloquea las tres señales en el hilo que llama: hay que crearlo antes
    /// de cualquier otro hilo, para que todos las tengan bloqueadas.
    SignalWatcher(CancelToken& shutdown, bot::Log& log);
    ~SignalWatcher();
    SignalWatcher(const SignalWatcher&) = delete;
    SignalWatcher& operator=(const SignalWatcher&) = delete;
    SignalWatcher(SignalWatcher&&) = delete;
    SignalWatcher& operator=(SignalWatcher&&) = delete;

    /// El bot que se apaga con la señal y la recarga de SIGHUP (si llegó
    /// uno mientras arrancaba, se aplica ahora). watch(nullptr, {}) espera a
    /// que termine un apagado o una recarga que ya empezó: después se puede
    /// destruir el bot.
    void watch(bot::Bot* bot, Reload reload);

    /// Manda la señal al hilo de señales (para las pruebas y para apagar).
    void send(int signal);

private:
    void wait_signals();
    void reload_loop();
    void request_reload();

    CancelToken& shutdown_;
    bot::Log& log_;
    sigset_t signals_{};
    std::atomic<bool> finished_{false};
    std::atomic<bool> disabled_{false}; ///< La recarga pidió apagar.
    std::atomic<bool> done_{false};

    /// Lo comparten el hilo de señales y el de recargas; se toma un momento.
    std::mutex wake_mutex_;
    std::condition_variable wake_;
    bool requested_ = false;
    bool stopping_ = false;

    /// Lo toma el hilo de recargas mientras recarga y watch.
    std::mutex reload_mutex_;
    Reload reload_;
    bool pending_reload_ = false; ///< Llegó un SIGHUP antes de que hubiera bot.

    /// Lo toma el hilo de señales mientras apaga el bot y watch. Aparte del
    /// de recargas: el apagado del bot no espera a la recarga, corren a la vez.
    std::mutex bot_mutex_;
    bot::Bot* bot_ = nullptr;

    std::thread reload_thread_;
    std::thread signal_thread_;
};

} // namespace chatbot::telegram

#endif // CHATBOT_TELEGRAM_SIGNAL_WATCHER_H
