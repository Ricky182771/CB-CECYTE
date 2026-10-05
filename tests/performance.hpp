#ifndef CHATBOT_TEST_PERFORMANCE_HPP
#define CHATBOT_TEST_PERFORMANCE_HPP

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdlib>
#include <string_view>

namespace chatbot_test {

inline bool strict_performance() {
    const char* value = std::getenv("CHATBOT_STRICT_PERF");
    return value != nullptr && std::string_view{value} == "1";
}

inline void report_latency(std::string_view label, std::chrono::steady_clock::duration elapsed) {
    const double ms = std::chrono::duration<double, std::milli>(elapsed).count();
    WARN(label << ": " << ms << " ms");
    if (strict_performance()) {
        CHECK(ms < 2000.0);
    }
}

} // namespace chatbot_test

#endif
