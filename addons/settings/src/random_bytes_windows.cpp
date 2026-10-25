#include "random_bytes.h"

namespace chatbot::addon_settings::detail {

std::optional<std::string> random_bytes(std::span<unsigned char> /*out*/) {
    return std::string{"no disponible en Windows"};
}

} // namespace chatbot::addon_settings::detail
