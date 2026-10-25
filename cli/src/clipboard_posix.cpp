// Portapapeles en POSIX: sin API del sistema (los programas los lanza
// run_with_input, en child_process_posix.cpp). CMake lo compila en lugar de
// clipboard_windows.cpp fuera de Windows.

#include "clipboard.h"

namespace chatbot::cli {

bool copy_to_native_clipboard(std::string_view /*text*/) {
    return false; // Sin API de portapapeles del sistema: programas u OSC 52.
}

std::optional<std::string> read_native_clipboard() {
    return std::nullopt; // Solo Windows: en POSIX la terminal pega sola.
}

} // namespace chatbot::cli
