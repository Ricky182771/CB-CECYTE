// Capa de plataforma para POSIX (Linux, Termux). CMake la compila en lugar
// de platform_windows.cpp fuera de Windows.

#include "chatbot/platform.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <system_error>

namespace chatbot {

namespace {

std::string errno_text() { return std::strerror(errno); }

/// Mensaje de errno en español para los errores comunes al descargar.
std::string describe_errno(int error) {
    switch (error) {
    case EACCES:
    case EPERM:
        return "permiso denegado";
    case ENOENT:
        return "la carpeta no existe";
    case ENOTDIR:
        return "una parte de la ruta no es una carpeta";
    case ENOSPC:
        return "no queda espacio en el disco";
    case EROFS:
        return "el sistema de archivos es de solo lectura";
    case ENAMETOOLONG:
        return "el nombre es demasiado largo";
    default:
        return std::strerror(error);
    }
}

bool write_all(int fd, std::string_view data) {
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

/// Permisos para el .tmp según FilePrivacy (ver platform.h).
mode_t file_mode(const std::string& path, FilePrivacy privacy) {
    if (privacy == FilePrivacy::Private) {
        return 0600;
    }
    struct stat info {};
    if (::stat(path.c_str(), &info) == 0) {
        return info.st_mode & 0777; // Se conservan los permisos que tenía.
    }
    return 0644;
}

} // namespace

Os current_os() { return Os::Posix; }

std::optional<std::tm> local_time(std::time_t time) {
    std::tm local{};
    if (localtime_r(&time, &local) == nullptr) {
        return std::nullopt;
    }
    return local;
}

bool stdio_is_terminal() {
    return ::isatty(STDIN_FILENO) == 1 && ::isatty(STDOUT_FILENO) == 1;
}

bool stdout_is_terminal() { return ::isatty(STDOUT_FILENO) == 1; }

std::optional<std::string> known_folder(KnownFolder /*folder*/) { return std::nullopt; }

// La terminal de POSIX no tiene página de códigos que cambiar.
ConsoleCodePages set_console_utf8() { return {}; }

void restore_console_code_pages(const ConsoleCodePages& /*previous*/) {}

CreateResult create_new_file(const std::string& path, std::string_view content) {
    // 0644 menos la umask del usuario: nunca ejecutable ni más abierto.
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (fd < 0) {
        if (errno == EEXIST) {
            return {CreateResult::Status::AlreadyExists, {}};
        }
        return {CreateResult::Status::Failed, describe_errno(errno)};
    }
    bool ok = write_all(fd, content);
    const int error = ok ? 0 : errno;
    ok = ::close(fd) == 0 && ok;
    if (!ok) {
        ::unlink(path.c_str());
        return {CreateResult::Status::Failed, describe_errno(error != 0 ? error : EIO)};
    }
    return {CreateResult::Status::Created, {}};
}

CreateResult create_directory(const std::string& path) {
    // 0755 menos la umask del usuario: si la umask es más estricta, se respeta.
    if (::mkdir(path.c_str(), 0755) == 0) {
        return {CreateResult::Status::Created, {}};
    }
    if (errno == EEXIST) {
        return {CreateResult::Status::AlreadyExists, {}};
    }
    return {CreateResult::Status::Failed, describe_errno(errno)};
}

SecretFileState check_secret_file(const std::string& path) {
    struct stat info {};
    if (::stat(path.c_str(), &info) != 0) {
        return errno == ENOENT ? SecretFileState::Missing : SecretFileState::Unreadable;
    }
    return (info.st_mode & (S_IRWXG | S_IRWXO)) != 0 ? SecretFileState::OpenAccess
                                                     : SecretFileState::Private;
}

std::optional<std::string> create_private_directory(const std::string& path,
                                                    FolderPrivacy privacy) {
    namespace fs = std::filesystem;
    const fs::path dir{path};
    std::error_code error;
    if (privacy == FolderPrivacy::PrivateIfNew && fs::is_directory(dir, error)) {
        return std::nullopt;
    }
    if (!dir.parent_path().empty()) {
        fs::create_directories(dir.parent_path(), error);
        if (error) {
            return "no se pudo crear " + dir.parent_path().string() + ": " + error.message();
        }
    }
    if (::mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) {
        return "no se pudo crear " + dir.string() + ": " + errno_text();
    }
    if (privacy == FolderPrivacy::Private && ::chmod(dir.c_str(), 0700) != 0) {
        return "no se pudieron ajustar los permisos de " + dir.string() + ": " + errno_text();
    }
    return std::nullopt;
}

std::optional<std::string> write_file_atomic(const std::string& path, std::string_view content,
                                             FilePrivacy file, FolderPrivacy folder) {
    namespace fs = std::filesystem;
    const fs::path dir = fs::path{path}.parent_path();
    if (!dir.empty()) {
        if (std::optional<std::string> error = create_private_directory(dir.string(), folder)) {
            return error;
        }
    }
    const mode_t mode = file_mode(path, file);
    const std::string temporary = path + ".tmp";
    const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (fd < 0) {
        return "no se pudo escribir " + temporary + ": " + errno_text();
    }
    // fchmod: la umask del proceso no debe abrir ni cerrar los permisos
    // pedidos, y si el .tmp ya existía con otros, O_TRUNC los conserva.
    const bool ok = ::fchmod(fd, mode) == 0 && write_all(fd, content) && ::fsync(fd) == 0;
    const std::string failure = ok ? std::string{} : errno_text();
    if (::close(fd) != 0 || !ok) {
        const std::string text = ok ? errno_text() : failure;
        ::unlink(temporary.c_str());
        return "no se pudo escribir " + temporary + ": " + text;
    }
    if (::rename(temporary.c_str(), path.c_str()) != 0) {
        const std::string text = errno_text();
        ::unlink(temporary.c_str());
        return "no se pudo reemplazar " + path + ": " + text;
    }
    // fsync de la carpeta: el rename queda en disco aunque se vaya la luz.
    const int dir_fd = ::open(dir.empty() ? "." : dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dir_fd >= 0) {
        ::fsync(dir_fd);
        ::close(dir_fd);
    }
    return std::nullopt;
}

} // namespace chatbot
