#include "chatbot/sleeper.h"

#include <thread>

namespace chatbot {

void RealSleeper::sleep_for(std::chrono::milliseconds duration) {
    std::this_thread::sleep_for(duration);
}

} // namespace chatbot
