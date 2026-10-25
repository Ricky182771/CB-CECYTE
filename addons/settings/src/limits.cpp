#include "chatbot/addon_settings/limits.h"

#include <algorithm>

namespace chatbot::addon_settings {
namespace {

/// "<qué> de <min> a <max> <unidad>." si value no está en el rango.
std::optional<std::string> check_range(std::int64_t value, std::int64_t min, std::int64_t max,
                                       std::string_view what, std::string_view unit) {
    if (value >= min && value <= max) {
        return std::nullopt;
    }
    std::string text = std::string{what} + " de " + std::to_string(min) + " a " +
                       std::to_string(max);
    if (!unit.empty()) {
        text += " " + std::string{unit};
    }
    return text + ".";
}

} // namespace

bool is_invite_code_char(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == '_' || c == '-';
}

std::optional<std::string> check_allowed_users(const std::vector<std::int64_t>& users) {
    for (const std::int64_t user : users) {
        if (user <= 0) {
            return "Los IDs de usuario autorizados deben ser números mayores que 0 (" +
                   std::to_string(user) + ").";
        }
    }
    return std::nullopt;
}

std::optional<std::string> check_invite_code(std::string_view code) {
    if (code.empty()) {
        return std::nullopt;
    }
    if (code.size() < kMinInviteCodeLength || code.size() > kMaxInviteCodeLength) {
        return "El código de invitación debe tener de " + std::to_string(kMinInviteCodeLength) +
               " a " + std::to_string(kMaxInviteCodeLength) + " caracteres.";
    }
    if (!std::all_of(code.begin(), code.end(), is_invite_code_char)) {
        return std::string{"El código de invitación solo puede tener letras sin acento (A-Z, "
                           "a-z), números, '_' y '-'."};
    }
    return std::nullopt;
}

std::optional<std::string> check_idle_minutes(std::int64_t minutes) {
    return check_range(minutes, kMinIdleTimeout.count(), kMaxIdleTimeout.count(),
                       "El tiempo para borrar una conversación inactiva debe ser", "minutos");
}

std::optional<std::string> check_daily_limit(std::int64_t messages) {
    return check_range(messages, 1, static_cast<std::int64_t>(kMaxDailyMessages),
                       "El tope de mensajes por usuario por día debe ser", "");
}

std::optional<std::string> check_concurrent_responses(std::int64_t responses) {
    return check_range(responses, 1, static_cast<std::int64_t>(kMaxConcurrentResponses),
                       "Las respuestas simultáneas deben ser", "");
}

std::optional<std::string> check_stale_minutes(std::int64_t minutes) {
    return check_range(minutes, kMinStaleAfter.count(), kMaxStaleAfter.count(),
                       "Los minutos para considerar viejo un mensaje deben ser", "");
}

std::optional<std::string> check_draft_interval_ms(std::int64_t milliseconds) {
    return check_range(milliseconds, kMinDraftInterval.count(), kMaxDraftInterval.count(),
                       "La frecuencia de la vista previa debe ser", "ms");
}

} // namespace chatbot::addon_settings
