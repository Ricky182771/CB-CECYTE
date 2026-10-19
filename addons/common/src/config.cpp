#include "chatbot/bot/config.h"

#include "chatbot/error.h"

#include <algorithm>
#include <optional>
#include <string>

namespace chatbot::bot {
namespace {

ChatError config_error(std::string message) {
    return ChatError{ErrorKind::Config, 0, std::move(message), std::nullopt};
}

bool is_code_char(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == '_' || c == '-';
}

} // namespace

Result<ValidBotConfig> validate_bot_config(BotConfig config) {
    for (const UserId user : config.allowed_users) {
        if (user <= 0) {
            return config_error("Los IDs de usuario autorizados deben ser números mayores que 0 (" +
                                std::to_string(user) + ").");
        }
    }
    const std::string& code = config.invite_code;
    if (!code.empty()) {
        if (code.size() < kMinInviteCodeLength || code.size() > kMaxInviteCodeLength) {
            return config_error("El código de invitación debe tener de " +
                                std::to_string(kMinInviteCodeLength) + " a " +
                                std::to_string(kMaxInviteCodeLength) + " caracteres.");
        }
        if (!std::all_of(code.begin(), code.end(), is_code_char)) {
            return config_error(
                "El código de invitación solo puede tener letras sin acento (A-Z, a-z), "
                "números, '_' y '-'.");
        }
    }
    if (config.session_idle_timeout < kMinIdleTimeout ||
        config.session_idle_timeout > kMaxIdleTimeout) {
        return config_error("El tiempo para borrar una conversación inactiva debe ser de " +
                            std::to_string(kMinIdleTimeout.count()) + " a " +
                            std::to_string(kMaxIdleTimeout.count()) + " minutos.");
    }
    if (config.daily_message_limit < 1 || config.daily_message_limit > kMaxDailyMessages) {
        return config_error("El tope de mensajes por usuario por día debe ser de 1 a " +
                            std::to_string(kMaxDailyMessages) + ".");
    }
    if (config.max_concurrent_responses < 1 ||
        config.max_concurrent_responses > kMaxConcurrentResponses) {
        return config_error("Las respuestas simultáneas deben ser de 1 a " +
                            std::to_string(kMaxConcurrentResponses) + ".");
    }
    if (config.stale_after < kMinStaleAfter || config.stale_after > kMaxStaleAfter) {
        return config_error("Los minutos para considerar viejo un mensaje deben ser de " +
                            std::to_string(kMinStaleAfter.count()) + " a " +
                            std::to_string(kMaxStaleAfter.count()) + ".");
    }
    if (config.draft_interval < kMinDraftInterval || config.draft_interval > kMaxDraftInterval) {
        return config_error("La frecuencia de la vista previa debe ser de " +
                            std::to_string(kMinDraftInterval.count()) + " a " +
                            std::to_string(kMaxDraftInterval.count()) + " ms.");
    }
    if (const std::optional<std::string> problem = validate_system_prompt(config.system_prompt)) {
        return config_error("Instrucciones del sistema: " + *problem);
    }
    return ValidBotConfig{std::move(config)};
}

} // namespace chatbot::bot
