#ifndef CHATBOT_ADDON_SETTINGS_TOKEN_H
#define CHATBOT_ADDON_SETTINGS_TOKEN_H

#include "chatbot/result.h"

#include <cstddef>
#include <string>
#include <string_view>

namespace chatbot::addon_settings {

/// true si el token tiene la forma de uno de @BotFather: dígitos, ':' y de
/// 20 a 100 caracteres A-Z, a-z, 0-9, '_' o '-'. Así nunca se arma una URL
/// con algo que no sea un token (/, ?, #, espacios).
[[nodiscard]] bool plausible_token(std::string_view token);

/// Largo del código que da generate_invite_code.
inline constexpr std::size_t kGeneratedInviteCodeLength = 16;

/// Un código de invitación nuevo: kGeneratedInviteCodeLength caracteres
/// A-Z, a-z, 0-9, '_' y '-' (los 64 de un enlace t.me/<bot>?start=…, así que
/// cada uno sale de 6 bits sin sesgo) con getrandom. Error Config si el
/// sistema no da números al azar; en Windows, "no disponible".
[[nodiscard]] Result<std::string> generate_invite_code();

} // namespace chatbot::addon_settings

#endif // CHATBOT_ADDON_SETTINGS_TOKEN_H
