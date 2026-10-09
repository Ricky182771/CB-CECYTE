// Partes de la capa de plataforma que son iguales en todos los sistemas
// (funciones puras): se compilan siempre, junto con platform_posix.cpp o
// platform_windows.cpp.

#include "chatbot/platform.h"

namespace chatbot {

unsigned interactive_console_input_mode(unsigned previous) {
    return previous & ~kConsoleProcessedInput;
}

std::string join_windows_path(std::string_view base, std::string_view tail) {
    while (!base.empty() && (base.back() == '\\' || base.back() == '/')) {
        base.remove_suffix(1);
    }
    return std::string{base} + "\\" + std::string{tail};
}

} // namespace chatbot
