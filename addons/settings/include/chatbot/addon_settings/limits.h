#ifndef CHATBOT_ADDON_SETTINGS_LIMITS_H
#define CHATBOT_ADDON_SETTINGS_LIMITS_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/// Rangos de la configuración de los bots y un validador por campo, en un
/// solo lugar: los usan el bot (validate_bot_config) y la pantalla de
/// configuración de la app (F2 → Addons), que muestra el texto del error
/// debajo del campo. Cada validador devuelve ese texto en español, o nullopt.
namespace chatbot::addon_settings {

inline constexpr std::chrono::minutes kMinIdleTimeout{1};
inline constexpr std::chrono::minutes kMaxIdleTimeout{24 * 60};
inline constexpr std::size_t kMaxDailyMessages = 10000;
inline constexpr std::size_t kMaxConcurrentResponses = 32;
inline constexpr std::chrono::minutes kMinStaleAfter{1};
inline constexpr std::chrono::minutes kMaxStaleAfter{24 * 60};
/// Telegram pide no mandar más de un mensaje por segundo por chat; los
/// borradores no tienen tope publicado, así que no se baja de 1/4 s.
inline constexpr std::chrono::milliseconds kMinDraftInterval{250};
inline constexpr std::chrono::milliseconds kMaxDraftInterval{10000};
/// Largo del código de invitación: el de /start <código> y el de los enlaces
/// t.me/<bot>?start=<código>, que Telegram limita a 64 caracteres A-Z, a-z,
/// 0-9, '_' y '-'. Al menos 8, para que no se adivine probando.
inline constexpr std::size_t kMinInviteCodeLength = 8;
inline constexpr std::size_t kMaxInviteCodeLength = 64;

/// true si c puede ir en un código de invitación: A-Z, a-z, 0-9, '_' o '-'.
[[nodiscard]] bool is_invite_code_char(char c);

/// IDs de Telegram autorizados: todos mayores que 0.
[[nodiscard]] std::optional<std::string> check_allowed_users(
    const std::vector<std::int64_t>& users);
/// Código de invitación: vacío (sin código) o de kMinInviteCodeLength a
/// kMaxInviteCodeLength caracteres de is_invite_code_char. El texto nunca
/// repite el código.
[[nodiscard]] std::optional<std::string> check_invite_code(std::string_view code);
/// Minutos sin mensajes para borrar una conversación.
[[nodiscard]] std::optional<std::string> check_idle_minutes(std::int64_t minutes);
/// Mensajes al modelo por usuario por día: de 1 a kMaxDailyMessages.
[[nodiscard]] std::optional<std::string> check_daily_limit(std::int64_t messages);
/// Respuestas a la vez en todo el bot: de 1 a kMaxConcurrentResponses.
[[nodiscard]] std::optional<std::string> check_concurrent_responses(std::int64_t responses);
/// Minutos para considerar viejo un mensaje.
[[nodiscard]] std::optional<std::string> check_stale_minutes(std::int64_t minutes);
/// Milisegundos entre vistas previas.
[[nodiscard]] std::optional<std::string> check_draft_interval_ms(std::int64_t milliseconds);

} // namespace chatbot::addon_settings

#endif // CHATBOT_ADDON_SETTINGS_LIMITS_H
