#ifndef CHATBOT_TEST_POSIX_PERMISSIONS_HPP
#define CHATBOT_TEST_POSIX_PERMISSIONS_HPP

// Permisos POSIX en las pruebas. En Windows no hay bits de permisos: manda
// la ACL del perfil del usuario. set_mode no hace nada ahí, y las pruebas
// que revisan modos (0600, 0700, umask, "chmod 600") van dentro de
// #ifndef _WIN32.

#include <filesystem>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace chatbot_test {

/// chmod en POSIX; en Windows no hace nada.
inline void set_mode(const std::filesystem::path& path, unsigned mode) {
#ifndef _WIN32
    ::chmod(path.c_str(), static_cast<mode_t>(mode));
#else
    (void)path;
    (void)mode;
#endif
}

} // namespace chatbot_test

#endif // CHATBOT_TEST_POSIX_PERMISSIONS_HPP
