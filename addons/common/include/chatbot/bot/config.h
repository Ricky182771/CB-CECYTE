#ifndef CHATBOT_BOT_CONFIG_H
#define CHATBOT_BOT_CONFIG_H

#include "chatbot/bot/types.h"

#include "chatbot/result.h"
#include "chatbot/system_prompt.h"

#include <chrono>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace chatbot::bot {

// Rangos válidos de BotConfig (validate_bot_config).
inline constexpr std::chrono::minutes kMinIdleTimeout{1};
inline constexpr std::chrono::minutes kMaxIdleTimeout{24 * 60};
inline constexpr std::size_t kMaxDailyMessages = 10000;
inline constexpr std::size_t kMaxConcurrentResponses = 32;
/// Largo del código de invitación: el de /start <código> y el de los enlaces
/// t.me/<bot>?start=<código>, que Telegram limita a 64 caracteres A-Z, a-z,
/// 0-9, '_' y '-'. Al menos 8, para que no se adivine probando.
inline constexpr std::size_t kMinInviteCodeLength = 8;
inline constexpr std::size_t kMaxInviteCodeLength = 64;

/// Todo lo que decide el comportamiento del bot. Los valores predeterminados
/// dejan el acceso cerrado: sin allowed_users ni invite_code nadie entra.
/// Leerla de un archivo o del entorno es de cada addon.
struct BotConfig {
    /// Usuarios que pueden usar el bot (IDs > 0).
    std::vector<UserId> allowed_users;
    /// /start <código> autoriza a quien lo mande (vacío: sin código). Esos
    /// usuarios viven en memoria: se pierden al reiniciar el bot.
    std::string invite_code;
    /// Instrucciones del sistema ya resueltas (como resolve_system_prompt en
    /// la app); vacías: sin mensaje de sistema.
    std::string system_prompt{kDefaultSystemPrompt};
    /// Una conversación sin mensajes durante este tiempo se borra.
    std::chrono::minutes session_idle_timeout{60};
    /// Mensajes que cada usuario puede mandar al modelo por día (fecha local).
    std::size_t daily_message_limit = 50;
    /// Respuestas que se preparan a la vez en todo el bot; las demás esperan
    /// su turno en orden de llegada.
    std::size_t max_concurrent_responses = 4;
    /// Límite del historial que se envía (trim_history; 0 = sin límite).
    std::size_t history_limit_bytes = 32000;
};

/// Configuración que ya pasó validate_bot_config: la única que acepta Bot.
class ValidBotConfig {
public:
    [[nodiscard]] const BotConfig& get() const { return config_; }

private:
    friend Result<ValidBotConfig> validate_bot_config(BotConfig config);
    explicit ValidBotConfig(BotConfig config) : config_(std::move(config)) {}

    BotConfig config_;
};

/// Revisa los rangos: IDs > 0; código vacío o de kMinInviteCodeLength a
/// kMaxInviteCodeLength caracteres A-Z, a-z, 0-9, '_' o '-';
/// session_idle_timeout de kMinIdleTimeout a kMaxIdleTimeout;
/// daily_message_limit de 1 a kMaxDailyMessages; max_concurrent_responses de
/// 1 a kMaxConcurrentResponses; system_prompt con validate_system_prompt.
/// Fuera de rango: error Config con el motivo en español (nunca el código).
[[nodiscard]] Result<ValidBotConfig> validate_bot_config(BotConfig config);

} // namespace chatbot::bot

#endif // CHATBOT_BOT_CONFIG_H
