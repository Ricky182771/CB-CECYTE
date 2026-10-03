#include "chatbot/sleeper.h"

#include <thread>

namespace chatbot {

bool RealSleeper::sleep_for(std::chrono::milliseconds duration, const CancelToken* cancel) {
    if (cancel != nullptr) {
        return !cancel->wait_for(duration);
    }
    std::this_thread::sleep_for(duration);
    return true;
}

} // namespace chatbot
