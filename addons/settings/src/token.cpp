#include "chatbot/addon_settings/token.h"

#include "random_bytes.h"

#include "chatbot/addon_settings/limits.h"

#include "chatbot/error.h"

#include <algorithm>
#include <array>
#include <optional>

namespace chatbot::addon_settings {
namespace {

/// Los 64 caracteres de is_invite_code_char: un byte & 63 elige uno sin sesgo.
constexpr std::string_view kCodeAlphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-";
static_assert(kCodeAlphabet.size() == 64);

} // namespace

bool plausible_token(std::string_view token) {
    const std::size_t colon = token.find(':');
    if (colon == 0 || colon == std::string_view::npos || colon > 20) {
        return false;
    }
    for (std::size_t i = 0; i < colon; ++i) {
        if (token[i] < '0' || token[i] > '9') {
            return false;
        }
    }
    const std::string_view secret = token.substr(colon + 1);
    if (secret.size() < 20 || secret.size() > 100) {
        return false;
    }
    return std::all_of(secret.begin(), secret.end(), is_invite_code_char);
}

Result<std::string> generate_invite_code() {
    std::array<unsigned char, kGeneratedInviteCodeLength> bytes{};
    if (const std::optional<std::string> error = detail::random_bytes(bytes)) {
        return ChatError{ErrorKind::Config, 0, "No se pudo generar el código: " + *error,
                         std::nullopt};
    }
    std::string code;
    code.reserve(bytes.size());
    for (const unsigned char byte : bytes) {
        code += kCodeAlphabet[byte & 63U];
    }
    return code;
}

} // namespace chatbot::addon_settings
