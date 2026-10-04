#include "atomic_file.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <system_error>

namespace chatbot {

namespace {

std::string errno_text() { return std::strerror(errno); }

bool write_all(int fd, const std::string& data) {
    std::size_t written = 0;
    while (written < data.size()) {
        const ssize_t n = ::write(fd, data.data() + written, data.size() - written);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        written += static_cast<std::size_t>(n);
    }
    return true;
}

} // namespace

std::optional<std::string> write_file_atomic(const std::string& path, const std::string& content,
                                             mode_t file_mode, mode_t dir_mode) {
    namespace fs = std::filesystem;
    const fs::path target{path};
    const fs::path dir = target.parent_path();
    std::error_code error;
    if (!dir.empty()) {
        if (!dir.parent_path().empty()) {
            fs::create_directories(dir.parent_path(), error);
            if (error) {
                return "no se pudo crear " + dir.parent_path().string() + ": " + error.message();
            }
        }
        if (::mkdir(dir.c_str(), dir_mode) != 0 && errno != EEXIST) {
            return "no se pudo crear " + dir.string() + ": " + errno_text();
        }
        if (::chmod(dir.c_str(), dir_mode) != 0) {
            return "no se pudieron ajustar los permisos de " + dir.string() + ": " + errno_text();
        }
    }
    const std::string temporary = path + ".tmp";
    const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, file_mode);
    if (fd < 0) {
        return "no se pudo escribir " + temporary + ": " + errno_text();
    }
    // fchmod: el umask del proceso no debe abrir ni cerrar los permisos pedidos.
    const bool ok = ::fchmod(fd, file_mode) == 0 && write_all(fd, content) && ::fsync(fd) == 0;
    const std::string failure = ok ? std::string{} : errno_text();
    if (::close(fd) != 0 || !ok) {
        ::unlink(temporary.c_str());
        return "no se pudo escribir " + temporary + ": " + (ok ? errno_text() : failure);
    }
    if (::rename(temporary.c_str(), path.c_str()) != 0) {
        const std::string text = errno_text();
        ::unlink(temporary.c_str());
        return "no se pudo reemplazar " + path + ": " + text;
    }
    // fsync del directorio: el rename queda en disco aunque se vaya la luz.
    const int dir_fd = ::open(dir.empty() ? "." : dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dir_fd >= 0) {
        ::fsync(dir_fd);
        ::close(dir_fd);
    }
    return std::nullopt;
}

} // namespace chatbot
