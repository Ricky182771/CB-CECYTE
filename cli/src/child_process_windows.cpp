// Programas externos en Windows: solo lo mínimo para compilar. Ahí no se
// lanzan programas (el portapapeles usa la API de Win32 y los addons no
// están disponibles). CMake lo compila en lugar de child_process_posix.cpp.

#include "child_process.h"

namespace chatbot::cli {

ProcessResult run_process(const std::vector<std::string>& /*argv*/,
                          const ProcessOptions& /*options*/) {
    return ProcessResult{};
}

bool is_executable_file(const std::string& /*path*/) {
    return false;
}

std::string exec_program(const std::string& /*path*/, const std::vector<std::string>& /*argv*/) {
    return "no disponible en Windows";
}

std::optional<unsigned long> current_user_id() {
    return std::nullopt;
}

std::optional<std::string> find_in_path(std::string_view /*program*/) {
    return std::nullopt;
}

} // namespace chatbot::cli
