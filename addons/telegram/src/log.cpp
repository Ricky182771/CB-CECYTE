#include "chatbot/telegram/log.h"

#include "chatbot/platform.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <string>

namespace chatbot::telegram {

void StreamLog::write(bot::LogLevel level, std::string_view line) {
    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    char stamp[32] = "(sin hora)";
    if (const std::optional<std::tm> local = local_time(now)) {
        std::strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", &*local);
    }
    const char* name = level == bot::LogLevel::Error     ? "error"
                       : level == bot::LogLevel::Warning ? "aviso"
                                                         : "info";
    std::string clean{line};
    for (char& c : clean) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte < 0x20 || byte == 0x7F) {
            c = ' ';
        }
    }
    const std::lock_guard<std::mutex> lock(mutex_);
    out_ << stamp << ' ' << name << ' ' << clean << '\n';
    out_.flush();
}

} // namespace chatbot::telegram
