#ifndef CHATBOT_TELEGRAM_LOG_H
#define CHATBOT_TELEGRAM_LOG_H

#include "chatbot/bot/types.h"

#include <mutex>
#include <ostream>
#include <string_view>

namespace chatbot::telegram {

/// Registro del bot en un flujo (stderr: con systemd lo guarda journald).
/// Cada línea: "AAAA-MM-DD HH:MM:SS nivel evento clave=valor…", en la hora
/// local del proceso. Lo que se escribe ya viene sin texto de los usuarios
/// ni secretos (lo cuida quien llama); aquí solo se quitan los controles.
/// Segura entre hilos.
class StreamLog final : public bot::Log {
public:
    explicit StreamLog(std::ostream& out) : out_(out) {}
    void write(bot::LogLevel level, std::string_view line) override;

private:
    std::mutex mutex_;
    std::ostream& out_;
};

} // namespace chatbot::telegram

#endif // CHATBOT_TELEGRAM_LOG_H
