#ifndef CHATBOT_TEST_FAKE_SYSTEM_HPP
#define CHATBOT_TEST_FAKE_SYSTEM_HPP

// Procesos falsos para F2 → Addons, TelegramAddon y los addons.* de
// chatbot-rpc: nunca corre systemctl, loginctl ni el addon de verdad.

#include "service_control.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace chatbot_test {

/// El addon "instalado" que contesta --config-version.
inline constexpr const char* kAddonExecutable = "/home/ana/CB/build/chatbot-telegram";

inline chatbot::cli::ProcessResult exited(int code, std::string out = {}, std::string err = {}) {
    chatbot::cli::ProcessResult result;
    result.status = chatbot::cli::ProcessResult::Status::Exited;
    result.exit_code = code;
    result.out = std::move(out);
    result.err = std::move(err);
    return result;
}

/// Contesta según los argumentos y anota cada llamada; lo usa el hilo del
/// ServiceWorker. Con hold, un comando se queda esperando hasta release o
/// hasta que se cancele (como run_process).
class FakeSystem {
public:
    FakeSystem() {
        answers_[std::string{kAddonExecutable} + " --config-version"] = exited(0, "1\n");
        set_active(true);
        answers_["systemctl --user is-enabled chatbot-telegram.service"] = exited(0, "enabled\n");
        answers_["systemctl --user show -p ExecMainStatus,Result chatbot-telegram.service"] =
            exited(0, "ExecMainStatus=0\nResult=success\n");
        answers_["loginctl show-user 1000 -p Linger --value"] = exited(0, "no\n");
    }

    void set_active(bool active) {
        const std::lock_guard<std::mutex> lock(mutex_);
        answers_["systemctl --user is-active chatbot-telegram.service"] =
            active ? exited(0, "active\n") : exited(3, "inactive\n");
    }
    void set_enabled(bool enabled) {
        const std::lock_guard<std::mutex> lock(mutex_);
        answers_["systemctl --user is-enabled chatbot-telegram.service"] =
            enabled ? exited(0, "enabled\n") : exited(1, "disabled\n");
    }
    void answer(const std::string& command, chatbot::cli::ProcessResult result) {
        const std::lock_guard<std::mutex> lock(mutex_);
        answers_[command] = std::move(result);
    }
    /// command se queda esperando (release o cancelar) cada vez que corre.
    void hold(const std::string& command) {
        const std::lock_guard<std::mutex> lock(mutex_);
        held_ = command;
        released_ = false;
    }
    void release() {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            released_ = true;
        }
        changed_.notify_all();
    }
    /// Espera a que el comando de hold esté corriendo.
    bool wait_held(std::chrono::milliseconds timeout = std::chrono::seconds{10}) {
        std::unique_lock<std::mutex> lock(mutex_);
        return changed_.wait_for(lock, timeout, [this] { return holding_; });
    }

    chatbot::cli::ProcessRunner runner() {
        return [this](const std::vector<std::string>& argv,
                      const chatbot::cli::ProcessOptions& options) {
            std::string joined;
            for (const std::string& part : argv) {
                joined += (joined.empty() ? "" : " ") + part;
            }
            std::unique_lock<std::mutex> lock(mutex_);
            calls_.push_back(joined);
            if (!held_.empty() && joined == held_) {
                holding_ = true;
                changed_.notify_all();
                while (!released_ &&
                       (options.cancel == nullptr || !options.cancel->is_cancelled())) {
                    changed_.wait_for(lock, std::chrono::milliseconds{10});
                }
                holding_ = false;
                if (!released_) {
                    chatbot::cli::ProcessResult cancelled;
                    cancelled.status = chatbot::cli::ProcessResult::Status::Cancelled;
                    return cancelled;
                }
            }
            const auto found = answers_.find(joined);
            return found != answers_.end() ? found->second : exited(0);
        };
    }

    [[nodiscard]] std::vector<std::string> calls() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return calls_;
    }
    [[nodiscard]] bool called(const std::string& command) const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return std::find(calls_.begin(), calls_.end(), command) != calls_.end();
    }
    void clear() {
        const std::lock_guard<std::mutex> lock(mutex_);
        calls_.clear();
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::map<std::string, chatbot::cli::ProcessResult> answers_;
    std::vector<std::string> calls_;
    std::string held_;
    bool released_ = false;
    bool holding_ = false;
};

/// Las acciones sobre el servicio que se corrieron (sin las consultas).
inline std::vector<std::string> service_actions(const FakeSystem& system) {
    std::vector<std::string> actions;
    constexpr std::string_view kPrefix = "systemctl --user ";
    for (const std::string& call : system.calls()) {
        for (const std::string_view verb :
             {"daemon-reload", "enable ", "disable ", "reload ", "restart "}) {
            if (call.starts_with(std::string{kPrefix} + std::string{verb})) {
                actions.push_back(call);
                break;
            }
        }
    }
    return actions;
}

} // namespace chatbot_test

#endif // CHATBOT_TEST_FAKE_SYSTEM_HPP
