#include "chatbot/addon_settings/invited.h"

#include "chatbot/error.h"
#include "chatbot/platform.h"

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <system_error>

namespace chatbot::addon_settings {
namespace {

ChatError invited_error(std::string message) {
    return ChatError{ErrorKind::Config, 0, std::move(message), std::nullopt};
}

/// flock sobre <path>.lock mientras vive. El candado es del archivo abierto,
/// así que también separa dos hilos del mismo proceso (cada uno abre el
/// suyo); se suelta al cerrarlo, también si el proceso muere.
class FileLock {
public:
    FileLock(const std::string& path, bool exclusive) : path_(path + ".lock") {
        fd_ = ::open(path_.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd_ < 0) {
            error_ = "no se pudo abrir " + path_ + ": " + std::strerror(errno);
            return;
        }
        while (::flock(fd_, exclusive ? LOCK_EX : LOCK_SH) != 0) {
            if (errno != EINTR) {
                error_ = "no se pudo bloquear " + path_ + ": " + std::strerror(errno);
                return;
            }
        }
    }
    ~FileLock() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }
    FileLock(const FileLock&) = delete;
    FileLock& operator=(const FileLock&) = delete;

    [[nodiscard]] const std::optional<std::string>& error() const { return error_; }

private:
    std::string path_;
    int fd_ = -1;
    std::optional<std::string> error_;
};

/// Lee la lista con el candado ya tomado. Sin archivo, vacía.
Result<std::vector<std::int64_t>> read_locked(const std::string& path) {
    std::error_code error;
    if (!std::filesystem::exists(path, error)) {
        if (error) {
            return invited_error("No se pudo revisar " + path + ": " + error.message() + ".");
        }
        return std::vector<std::int64_t>{};
    }
    std::ifstream file{path, std::ios::binary};
    if (!file) {
        return invited_error("No se pudo leer " + path + ".");
    }
    const std::string text{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
    if (file.bad()) {
        return invited_error("No se pudo leer " + path + ".");
    }
    return detail::parse_invited(path, text);
}

/// Lee, deja que change cambie la lista y, si cambió, la escribe; todo bajo
/// el candado exclusivo.
template <typename Change>
Result<std::vector<std::int64_t>> update(const std::string& path, Change change) {
    try {
        const std::string folder = std::filesystem::path{path}.parent_path().string();
        if (std::optional<std::string> error =
                create_private_directory(folder, FolderPrivacy::PrivateIfNew)) {
            return invited_error("No se pudo preparar la carpeta de los invitados: " + *error);
        }
        const FileLock lock{path, true};
        if (lock.error().has_value()) {
            return invited_error("No se pudieron guardar los invitados: " + *lock.error());
        }
        Result<std::vector<std::int64_t>> users = read_locked(path);
        if (users.is_error()) {
            return users;
        }
        std::vector<std::int64_t> list = users.value();
        if (!change(list)) {
            return list;
        }
        if (std::optional<std::string> error =
                write_file_atomic(path, detail::format_invited(list), FilePrivacy::Private,
                                  FolderPrivacy::PrivateIfNew)) {
            return invited_error("No se pudieron guardar los invitados en " + path + ": " +
                                 *error);
        }
        return list;
    } catch (const std::exception&) {
        return invited_error("No se pudieron guardar los invitados en " + path + ".");
    }
}

} // namespace

Result<std::vector<std::int64_t>> load_invited(const std::string& path) {
    try {
        std::error_code error;
        const std::filesystem::path folder = std::filesystem::path{path}.parent_path();
        if (!std::filesystem::is_directory(folder, error)) {
            // Sin carpeta no hay invitados (y leer no la crea).
            return std::vector<std::int64_t>{};
        }
        const FileLock lock{path, false};
        if (lock.error().has_value()) {
            return invited_error("No se pudieron leer los invitados: " + *lock.error());
        }
        return read_locked(path);
    } catch (const std::exception&) {
        return invited_error("No se pudieron leer los invitados de " + path + ".");
    }
}

Result<std::vector<std::int64_t>> add_invited(const std::string& path, std::int64_t user) {
    return update(path, [user](std::vector<std::int64_t>& list) {
        if (std::find(list.begin(), list.end(), user) != list.end()) {
            return false;
        }
        list.push_back(user);
        return true;
    });
}

Result<std::vector<std::int64_t>> remove_invited(const std::string& path, std::int64_t user) {
    return update(path, [user](std::vector<std::int64_t>& list) {
        const auto found = std::find(list.begin(), list.end(), user);
        if (found == list.end()) {
            return false;
        }
        list.erase(found);
        return true;
    });
}

} // namespace chatbot::addon_settings
