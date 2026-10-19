#include "chatbot/bot/access.h"

namespace chatbot::bot {
namespace {

/// Compara sin salir antes: tarda lo mismo sin importar dónde difieren (sí
/// depende del largo de given, que no es secreto).
bool same_code(std::string_view expected, std::string_view given) {
    unsigned difference = expected.size() == given.size() ? 0U : 1U;
    for (std::size_t i = 0; i < given.size(); ++i) {
        const char wanted = i < expected.size() ? expected[i] : '\0';
        difference |= static_cast<unsigned>(static_cast<unsigned char>(wanted) ^
                                            static_cast<unsigned char>(given[i]));
    }
    return difference == 0;
}

} // namespace

AccessControl::AccessControl(const BotConfig& config, const std::vector<UserId>& invited)
    : allowed_(config.allowed_users.begin(), config.allowed_users.end()),
      invited_(invited.begin(), invited.end()), invite_code_(config.invite_code) {}

bool AccessControl::is_allowed(UserId user) const {
    return allowed_.count(user) > 0 || invited_.count(user) > 0;
}

bool AccessControl::redeem(UserId user, std::string_view code) {
    if (invite_code_.empty() || code.empty() || !same_code(invite_code_, code)) {
        return false;
    }
    invited_.insert(user);
    return true;
}

} // namespace chatbot::bot
