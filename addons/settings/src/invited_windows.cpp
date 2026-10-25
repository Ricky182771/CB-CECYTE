#include "chatbot/addon_settings/invited.h"
#include "chatbot/addon_settings/registry.h"

#include "chatbot/error.h"

#include <optional>

namespace chatbot::addon_settings {
namespace {

ChatError unavailable() {
    return ChatError{ErrorKind::Config, 0, std::string{kAddonsUnavailableOnWindows},
                     std::nullopt};
}

} // namespace

Result<std::vector<std::int64_t>> load_invited(const std::string& /*path*/) {
    return unavailable();
}

Result<std::vector<std::int64_t>> add_invited(const std::string& /*path*/, std::int64_t /*user*/) {
    return unavailable();
}

Result<std::vector<std::int64_t>> remove_invited(const std::string& /*path*/,
                                                 std::int64_t /*user*/) {
    return unavailable();
}

} // namespace chatbot::addon_settings
