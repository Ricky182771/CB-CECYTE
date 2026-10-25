#include "random_bytes.h"

#include <sys/random.h>

#include <cerrno>
#include <cstring>

namespace chatbot::addon_settings::detail {

std::optional<std::string> random_bytes(std::span<unsigned char> out) {
    std::size_t filled = 0;
    while (filled < out.size()) {
        // Sin GRND_NONBLOCK: espera solo si la fuente aún no arranca (al
        // inicio del sistema). EINTR: una señal; se vuelve a pedir.
        const ssize_t got = ::getrandom(out.data() + filled, out.size() - filled, 0);
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            return std::string{"getrandom falló: "} + std::strerror(errno);
        }
        filled += static_cast<std::size_t>(got);
    }
    return std::nullopt;
}

} // namespace chatbot::addon_settings::detail
