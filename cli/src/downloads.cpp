#include "downloads.h"

#include "chatbot/utf8.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <exception>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <utility>

namespace chatbot::cli {

namespace {

std::optional<std::string> non_empty(const EnvLookup& env, std::string_view name) {
    std::optional<std::string> value = env(name);
    return value.has_value() && !value->empty() ? value : std::nullopt;
}

bool is_dir(const std::string& path) {
    try {
        std::error_code error;
        return std::filesystem::is_directory(std::filesystem::path{path}, error);
    } catch (const std::exception&) {
        return false; // En Windows, fs::path lanza si la ruta no es UTF-8 válido.
    }
}

/// dir + separador + name: "/" en POSIX, "\\" en Windows.
std::string join_path(const std::string& dir, std::string_view name) {
    if (current_os() == Os::Windows) {
        return join_windows_path(dir, name);
    }
    return dir + "/" + std::string{name};
}

std::string lower(std::string_view text) {
    std::string out{text};
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

/// true si name es un dispositivo reservado de Windows (CON, PRN, AUX, NUL,
/// COM1 a COM9, LPT1 a LPT9) sin distinguir mayúsculas, también con
/// extensión ("con.cpp", "Aux.tar.gz"): Windows mira lo que va antes del
/// primer punto, sin los espacios del final ("con .txt").
bool is_reserved_windows_name(std::string_view name) {
    std::string_view stem = name.substr(0, name.find('.'));
    while (!stem.empty() && stem.back() == ' ') {
        stem.remove_suffix(1);
    }
    const std::string device = lower(stem);
    if (device == "con" || device == "prn" || device == "aux" || device == "nul") {
        return true;
    }
    return device.size() == 4 &&
           (device.compare(0, 3, "com") == 0 || device.compare(0, 3, "lpt") == 0) &&
           device[3] >= '1' && device[3] <= '9';
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

std::optional<std::string> resolve_windows_download_dir(
    const std::optional<std::string>& chat_download_dir,
    const std::optional<std::string>& downloads_folder,
    const std::optional<std::string>& user_profile) {
    for (const std::optional<std::string>* dir :
         {&chat_download_dir, &downloads_folder, &user_profile}) {
        if (dir->has_value() && !(*dir)->empty()) {
            return **dir;
        }
    }
    return std::nullopt;
}

std::optional<std::string> default_download_dir(const EnvLookup& env) {
    if (current_os() == Os::Windows) {
        return resolve_windows_download_dir(env("CHAT_DOWNLOAD_DIR"),
                                            known_folder(KnownFolder::Downloads),
                                            env("USERPROFILE"));
    }
    return resolve_download_dir(env);
}

std::string_view no_download_dir_message(Os os) {
    return os == Os::Windows ? kNoDownloadDirWindows : kNoDownloadDir;
}

std::string display_home(const EnvLookup& env, Os os) {
    if (os == Os::Windows) {
        return {};
    }
    return env("HOME").value_or("");
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
    // Reglas de Windows en todas las plataformas: lo guardado se puede
    // copiar de un sistema a otro.
    if (name.find_first_of("<>:\"|?*") != std::string_view::npos) {
        // ":" además crearía un flujo alterno de NTFS ("suma.cpp:x").
        return "El nombre no puede llevar < > : \" | ? * (Windows no los acepta).";
    }
    if (name.back() == '.' || name.back() == ' ') {
        return "El nombre no puede terminar en punto ni en espacio (Windows los quita).";
    }
    if (is_reserved_windows_name(name)) {
        return "El nombre \"" + std::string{name} +
               "\" está reservado en Windows (CON, PRN, AUX, NUL, COM1 a COM9 y LPT1 a LPT9, "
               "también con extensión).";
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
    const std::string dir = join_path(base, "chatbot");
    // 0755 menos la umask del usuario en POSIX; la ACL de base en Windows.
    const CreateResult created = create_directory(dir);
    switch (created.status) {
    case CreateResult::Status::Created:
        return {dir, ""};
    case CreateResult::Status::AlreadyExists:
        if (is_dir(dir)) {
            return {dir, ""};
        }
        return {"", "No se pudo crear " + dir + ": ya existe y no es una carpeta"};
    case CreateResult::Status::Failed:
        break;
    }
    return {"", "No se pudo crear " + dir + ": " + created.error};
}

WriteResult write_new_file(const std::string& dir, std::string_view name,
                           std::string_view content) {
    for (int attempt = 1; attempt <= kMaxNameSuffix; ++attempt) {
        const std::string candidate =
            join_path(dir, attempt == 1 ? std::string{name} : with_suffix(name, attempt));
        const CreateResult created = create_new_file(candidate, content);
        switch (created.status) {
        case CreateResult::Status::Created:
            return {candidate, ""};
        case CreateResult::Status::AlreadyExists:
            continue;
        case CreateResult::Status::Failed:
            break;
        }
        return {"", "No se pudo escribir " + candidate + ": " + created.error};
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
