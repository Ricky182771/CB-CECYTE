#ifndef CHATBOT_TEST_ENV_GUARD_HPP
#define CHATBOT_TEST_ENV_GUARD_HPP

#include <cstdlib>
#include <optional>
#include <string>
#include <utility>

namespace chatbot_test {

/// Cambia una variable de entorno del proceso mientras vive y luego la deja
/// como estaba. nullptr la borra. setenv/unsetenv en POSIX; _putenv_s en
/// Windows (con "" la borra, y getenv da nullptr).
class EnvGuard {
public:
    EnvGuard(std::string name, const char* value) : name_(std::move(name)) {
        if (const char* previous = std::getenv(name_.c_str())) {
            previous_ = previous;
        }
        set(value);
    }
    ~EnvGuard() { set(previous_ ? previous_->c_str() : nullptr); }
    EnvGuard(const EnvGuard&) = delete;
    EnvGuard& operator=(const EnvGuard&) = delete;

private:
    void set(const char* value) const {
#ifdef _WIN32
        ::_putenv_s(name_.c_str(), value != nullptr ? value : "");
#else
        if (value != nullptr) {
            ::setenv(name_.c_str(), value, 1);
        } else {
            ::unsetenv(name_.c_str());
        }
#endif
    }

    std::string name_;
    std::optional<std::string> previous_;
};

} // namespace chatbot_test

#endif // CHATBOT_TEST_ENV_GUARD_HPP
