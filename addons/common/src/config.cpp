#include "chatbot/bot/config.h"

#include "chatbot/error.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace chatbot::bot {

Result<ValidBotConfig> validate_bot_config(BotConfig config) {
    namespace limits = addon_settings;
    const std::optional<std::string> problems[] = {
        limits::check_allowed_users(config.allowed_users),
        limits::check_invite_code(config.invite_code),
        limits::check_idle_minutes(config.session_idle_timeout.count()),
        limits::check_daily_limit(static_cast<std::int64_t>(config.daily_message_limit)),
        limits::check_concurrent_responses(
            static_cast<std::int64_t>(config.max_concurrent_responses)),
        limits::check_stale_minutes(config.stale_after.count()),
        limits::check_draft_interval_ms(config.draft_interval.count()),
    };
    for (const std::optional<std::string>& problem : problems) {
        if (problem.has_value()) {
            return ChatError{ErrorKind::Config, 0, *problem, std::nullopt};
        }
    }
    if (const std::optional<std::string> problem = validate_system_prompt(config.system_prompt)) {
        return ChatError{ErrorKind::Config, 0, "Instrucciones del sistema: " + *problem,
                         std::nullopt};
    }
    return ValidBotConfig{std::move(config)};
}

} // namespace chatbot::bot
