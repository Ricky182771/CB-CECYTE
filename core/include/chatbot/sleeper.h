#ifndef CHATBOT_SLEEPER_H
#define CHATBOT_SLEEPER_H

#include <chrono>

namespace chatbot {

/// Espera inyectable para que las pruebas no esperen de verdad (sección 6).
class Sleeper {
public:
    virtual ~Sleeper() = default;
    virtual void sleep_for(std::chrono::milliseconds duration) = 0;
};

/// Implementación real: duerme el hilo llamante.
class RealSleeper final : public Sleeper {
public:
    void sleep_for(std::chrono::milliseconds duration) override;
};

} // namespace chatbot

#endif // CHATBOT_SLEEPER_H
