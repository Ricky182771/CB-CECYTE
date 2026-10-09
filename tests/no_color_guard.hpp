#ifndef CHATBOT_TEST_NO_COLOR_GUARD_HPP
#define CHATBOT_TEST_NO_COLOR_GUARD_HPP

#include "env_guard.hpp"

namespace chatbot_test {

/// Solo altera NO_COLOR; restaura su valor al terminar la prueba.
class NoColorGuard {
public:
    explicit NoColorGuard(const char* value) : guard_("NO_COLOR", value) {}

private:
    EnvGuard guard_;
};

} // namespace chatbot_test

#endif
