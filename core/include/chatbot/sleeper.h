#ifndef CHATBOT_SLEEPER_H
#define CHATBOT_SLEEPER_H

#include "chatbot/cancel_token.h"

#include <chrono>

namespace chatbot {

/// Espera inyectable para que las pruebas no esperen de verdad (sección 6).
class Sleeper {
public:
    virtual ~Sleeper() = default;

    /// Espera la duración indicada. Con token, la cancelación interrumpe la
    /// espera. Devuelve false si se interrumpió por cancelación.
    [[nodiscard]] virtual bool sleep_for(std::chrono::milliseconds duration,
                                         const CancelToken* cancel) = 0;
};

/// Implementación real: duerme el hilo llamante, o espera en el token si lo hay.
class RealSleeper final : public Sleeper {
public:
    [[nodiscard]] bool sleep_for(std::chrono::milliseconds duration,
                                 const CancelToken* cancel) override;
};

} // namespace chatbot

#endif // CHATBOT_SLEEPER_H
