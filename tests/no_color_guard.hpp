#ifndef CHATBOT_TEST_NO_COLOR_GUARD_HPP
#define CHATBOT_TEST_NO_COLOR_GUARD_HPP

#include <cstdlib>
#include <optional>
#include <string>

namespace chatbot_test {

/// Solo altera NO_COLOR; restaura su valor al terminar la prueba.
class NoColorGuard {
public:
    explicit NoColorGuard(const char* value) {
        if (const char* previous = std::getenv("NO_COLOR")) {
            previous_ = previous;
        }
        if (value != nullptr) {
            ::setenv("NO_COLOR", value, 1);
        } else {
            ::unsetenv("NO_COLOR");
        }
    }
    ~NoColorGuard() {
        if (previous_) {
            ::setenv("NO_COLOR", previous_->c_str(), 1);
        } else {
            ::unsetenv("NO_COLOR");
        }
    }
    NoColorGuard(const NoColorGuard&) = delete;
    NoColorGuard& operator=(const NoColorGuard&) = delete;

private:
    std::optional<std::string> previous_;
};

} // namespace chatbot_test

#endif
