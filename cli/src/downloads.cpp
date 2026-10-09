#include "downloads.h"

#include "chatbot/utf8.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace chatbot::cli {

namespace {

std::optional<std::string> non_empty(const EnvLookup& env, std::string_view name) {
    std::optional<std::string> value = env(name);
    return value.has_value() && !value->empty() ? value : std::nullopt;
}

bool is_dir(const std::string& path) {
    struct stat info{};
    return ::stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

std::string lower(std::string_view text) {
    std::string out{text};
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

/// Mensaje de errno en español para los errores comunes.
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

/// Nombre con sufijo: "suma.cpp" → "suma-2.cpp"; sin extensión, "suma-2".
std::string with_suffix(std::string_view name, int suffix) {
    const std::size_t dot = name.rfind('.');
    const std::string tag = "-" + std::to_string(suffix);
    if (dot == std::string_view::npos || dot == 0) {
        return std::string{name} + tag;
    }
    return std::string{name.substr(0, dot)} + tag + std::string{name.substr(dot)};
}

/// Escribe todo content en fd.
bool write_all(int fd, std::string_view content) {
    while (!content.empty()) {
        const ssize_t written = ::write(fd, content.data(), content.size());
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        content.remove_prefix(static_cast<std::size_t>(written));
    }
    return true;
}

} // namespace

std::optional<std::string> read_xdg_download_dir(const std::string& path,
                                                 const std::string& home) {
    std::ifstream file(path);
    if (!file) {
        return std::nullopt;
    }
    constexpr std::string_view kKey = "XDG_DOWNLOAD_DIR=";
    std::string line;
    std::optional<std::string> found;
    while (std::getline(file, line)) {
        std::string_view text = line;
        while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
            text.remove_prefix(1);
        }
        if (text.substr(0, kKey.size()) != kKey) {
            continue; // Comentarios y otras carpetas.
        }
        text.remove_prefix(kKey.size());
        while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
            text.remove_suffix(1);
        }
        if (text.size() < 2 || text.front() != '"' || text.back() != '"') {
            found.reset();
            continue;
        }
        text = text.substr(1, text.size() - 2);
        constexpr std::string_view kHome = "$HOME";
        if (text.substr(0, kHome.size()) == kHome &&
            (text.size() == kHome.size() || text[kHome.size()] == '/')) {
            if (home.empty()) {
                found.reset();
                continue;
            }
            found = home + std::string{text.substr(kHome.size())};
        } else if (!text.empty() && text.front() == '/') {
            found = std::string{text};
        } else {
            found.reset(); // Relativa u otra variable: no se usa.
        }
    }
    // Si la llave aparece más de una vez, vale la última (como en un shell).
    return found;
}

std::optional<std::string> resolve_download_dir(const EnvLookup& env) {
    if (std::optional<std::string> custom = non_empty(env, "CHAT_DOWNLOAD_DIR")) {
        return custom;
    }
    const std::optional<std::string> home = non_empty(env, "HOME");
    std::optional<std::string> config_home = non_empty(env, "XDG_CONFIG_HOME");
    if (!config_home.has_value() && home.has_value()) {
        config_home = *home + "/.config";
    }
    if (config_home.has_value()) {
        const std::optional<std::string> xdg =
            read_xdg_download_dir(*config_home + "/user-dirs.dirs", home.value_or(""));
        if (xdg.has_value() && is_dir(*xdg)) {
            return xdg;
        }
    }
    if (!home.has_value()) {
        return std::nullopt;
    }
    if (non_empty(env, "TERMUX_VERSION").has_value() && is_dir(*home + "/storage/downloads")) {
        return *home + "/storage/downloads";
    }
    for (const char* name : {"/Descargas", "/Downloads"}) {
        if (is_dir(*home + name)) {
            return *home + name;
        }
    }
    return home;
}

std::string extension_for(std::string_view language) {
    // La tabla de la especificación; cualquier otro lenguaje → ".txt".
    static constexpr std::array<std::pair<std::string_view, std::string_view>, 28> kTable{{
        {"cpp", ".cpp"},   {"c++", ".cpp"},       {"cc", ".cpp"},     {"c", ".c"},
        {"h", ".h"},       {"hpp", ".hpp"},       {"python", ".py"},  {"py", ".py"},
        {"bash", ".sh"},   {"sh", ".sh"},         {"shell", ".sh"},   {"js", ".js"},
        {"javascript", ".js"}, {"ts", ".ts"},     {"json", ".json"},  {"html", ".html"},
        {"css", ".css"},   {"java", ".java"},     {"cs", ".cs"},      {"csharp", ".cs"},
        {"rust", ".rs"},   {"rs", ".rs"},         {"go", ".go"},      {"sql", ".sql"},
        {"yaml", ".yaml"}, {"yml", ".yaml"},      {"md", ".md"},      {"markdown", ".md"},
    }};
    const std::string key = lower(language);
    for (const auto& [name, extension] : kTable) {
        if (key == name) {
            return std::string{extension};
        }
    }
    return ".txt";
}

std::optional<std::string> validate_file_name(std::string_view name) {
    if (name.empty()) {
        return "Falta el nombre del archivo.";
    }
    if (name.find('/') != std::string_view::npos || name.find('\\') != std::string_view::npos) {
        return "El nombre no puede llevar / ni \\: solo el nombre del archivo, sin carpetas.";
    }
    if (name.find("..") != std::string_view::npos) {
        return "El nombre no puede llevar \"..\".";
    }
    if (name.front() == '.') {
        return "El nombre no puede empezar con \".\" (sería un archivo oculto).";
    }
    if (name.size() > kMaxFileNameBytes) {
        return "El nombre mide más de " + std::to_string(kMaxFileNameBytes) + " bytes.";
    }
    std::size_t i = 0;
    while (i < name.size()) {
        const std::optional<std::uint32_t> code = utf8::next_code_point(name, i);
        if (!code.has_value()) {
            return "El nombre no es UTF-8 válido.";
        }
        if (*code < 0x20U || *code == 0x7FU || (*code >= 0x80U && *code <= 0x9FU)) {
            return "El nombre no puede llevar caracteres de control.";
        }
    }
    return std::nullopt;
}

std::string block_file_name(std::string_view user_name, int number, std::string_view language) {
    if (user_name.empty()) {
        return "bloque-" + std::to_string(number) + extension_for(language);
    }
    // validate_file_name ya descartó los nombres que empiezan con ".": un
    // punto en cualquier lugar es la extensión.
    if (user_name.find('.') != std::string_view::npos) {
        return std::string{user_name};
    }
    return std::string{user_name} + extension_for(language);
}

std::string with_final_newline(std::string_view text) {
    std::string out{text};
    if (out.empty() || out.back() != '\n') {
        out += '\n';
    }
    return out;
}

WriteResult ensure_download_dir(const std::string& base) {
    const std::string dir = base + "/chatbot";
    if (::mkdir(dir.c_str(), 0755) == 0) {
        // mkdir respeta la umask; la carpeta queda en 0755.
        if (::chmod(dir.c_str(), 0755) != 0) {
            return {"", "No se pudo preparar " + dir + ": " + describe_errno(errno)};
        }
        return {dir, ""};
    }
    const int error = errno;
    if (error == EEXIST && is_dir(dir)) {
        return {dir, ""};
    }
    if (error == EEXIST) {
        return {"", "No se pudo crear " + dir + ": ya existe y no es una carpeta"};
    }
    return {"", "No se pudo crear " + dir + ": " + describe_errno(error)};
}

WriteResult write_new_file(const std::string& dir, std::string_view name,
                           std::string_view content) {
    for (int attempt = 1; attempt <= kMaxNameSuffix; ++attempt) {
        const std::string candidate =
            dir + "/" + (attempt == 1 ? std::string{name} : with_suffix(name, attempt));
        const int fd =
            ::open(candidate.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0644);
        if (fd < 0) {
            if (errno == EEXIST) {
                continue;
            }
            return {"", "No se pudo escribir " + candidate + ": " + describe_errno(errno)};
        }
        // open respeta la umask; el archivo queda en 0644 (nunca ejecutable).
        bool ok = ::fchmod(fd, 0644) == 0 && write_all(fd, content);
        const int error = ok ? 0 : errno;
        ok = ::close(fd) == 0 && ok;
        if (!ok) {
            ::unlink(candidate.c_str());
            return {"", "No se pudo escribir " + candidate + ": " +
                            describe_errno(error != 0 ? error : EIO)};
        }
        return {candidate, ""};
    }
    return {"", "Ya existen " + std::string{name} + " y sus copias hasta -" +
                    std::to_string(kMaxNameSuffix) + " en " + dir + "."};
}

std::string display_path(const std::string& path, const std::string& home) {
    if (!home.empty() && home != "/" && path.size() > home.size() &&
        path.compare(0, home.size(), home) == 0 && path[home.size()] == '/') {
        return "~" + path.substr(home.size());
    }
    return path;
}

} // namespace chatbot::cli
