// Capa de plataforma para Windows. CMake la compila en lugar de
// platform_posix.cpp en Windows. Todas las llamadas a Win32 usan la versión W
// con conversión explícita de UTF-8 (platform_windows.h).

#include "chatbot/platform.h"
#include "chatbot/platform_windows.h"

#include <windows.h>
// initguid.h antes de knownfolders.h: los FOLDERID_* se definen aquí y no
// hace falta enlazar libuuid.
#include <initguid.h>
#include <knownfolders.h>
#include <objbase.h>
#include <shlobj.h>

#include <algorithm>
#include <climits>
#include <exception>
#include <filesystem>
#include <system_error>

namespace chatbot {

namespace {

/// Reintentos de MoveFileExW cuando otro proceso tiene abierto el archivo.
constexpr int kMoveRetries = 5;
constexpr DWORD kMoveRetryDelayMs = 50;

/// true si el handle estándar es una consola.
bool is_console(DWORD which) {
    const HANDLE handle = ::GetStdHandle(which);
    DWORD mode = 0;
    return handle != nullptr && handle != INVALID_HANDLE_VALUE &&
           ::GetConsoleMode(handle, &mode) != 0;
}

/// Escribe todo data en file. false con GetLastError si falla.
bool write_all(HANDLE file, std::string_view data) {
    while (!data.empty()) {
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(data.size(), 1U << 30U));
        DWORD written = 0;
        if (::WriteFile(file, data.data(), chunk, &written, nullptr) == 0) {
            return false;
        }
        data.remove_prefix(written);
    }
    return true;
}

} // namespace

std::wstring utf8_to_utf16(std::string_view text) {
    if (text.empty() || text.size() > static_cast<std::size_t>(INT_MAX)) {
        return {};
    }
    const int size = static_cast<int>(text.size());
    const int length = ::MultiByteToWideChar(CP_UTF8, 0, text.data(), size, nullptr, 0);
    if (length <= 0) {
        return {};
    }
    std::wstring out(static_cast<std::size_t>(length), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.data(), size, out.data(), length);
    return out;
}

std::string utf16_to_utf8(std::wstring_view text) {
    if (text.empty() || text.size() > static_cast<std::size_t>(INT_MAX)) {
        return {};
    }
    const int size = static_cast<int>(text.size());
    const int length =
        ::WideCharToMultiByte(CP_UTF8, 0, text.data(), size, nullptr, 0, nullptr, nullptr);
    if (length <= 0) {
        return {};
    }
    std::string out(static_cast<std::size_t>(length), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.data(), size, out.data(), length, nullptr, nullptr);
    return out;
}

std::string windows_error_text(unsigned long code) {
    wchar_t* buffer = nullptr;
    const DWORD length = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::wstring text;
    if (length > 0 && buffer != nullptr) {
        text.assign(buffer, length);
    }
    if (buffer != nullptr) {
        ::LocalFree(buffer);
    }
    // FormatMessage termina con ".\r\n": el código va después, sin el punto.
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' ||
                             text.back() == L' ' || text.back() == L'.')) {
        text.pop_back();
    }
    const std::string number = "código " + std::to_string(code);
    return text.empty() ? "error de Windows (" + number + ")"
                        : utf16_to_utf8(text) + " (" + number + ")";
}

Os current_os() { return Os::Windows; }

std::optional<std::tm> local_time(std::time_t time) {
    // MinGW (UCRT y msvcrt) declara la versión de Microsoft:
    // errno_t localtime_s(struct tm*, const time_t*), con el orden al revés
    // del anexo K de C11.
    std::tm local{};
    if (localtime_s(&local, &time) != 0) {
        return std::nullopt;
    }
    return local;
}

bool stdio_is_terminal() { return is_console(STD_INPUT_HANDLE) && is_console(STD_OUTPUT_HANDLE); }

bool stdout_is_terminal() { return is_console(STD_OUTPUT_HANDLE); }

std::optional<std::string> known_folder(KnownFolder folder) {
    const KNOWNFOLDERID* id = &FOLDERID_RoamingAppData;
    switch (folder) {
    case KnownFolder::RoamingAppData:
        id = &FOLDERID_RoamingAppData;
        break;
    case KnownFolder::LocalAppData:
        id = &FOLDERID_LocalAppData;
        break;
    case KnownFolder::Downloads:
        id = &FOLDERID_Downloads;
        break;
    }
    PWSTR path = nullptr;
    const HRESULT result = ::SHGetKnownFolderPath(*id, KF_FLAG_DEFAULT, nullptr, &path);
    std::optional<std::string> found;
    if (SUCCEEDED(result) && path != nullptr && path[0] != L'\0') {
        found = utf16_to_utf8(path);
    }
    ::CoTaskMemFree(path); // También si falló (la documentación lo pide).
    return found;
}

std::optional<std::string> create_private_directory(const std::string& path,
                                                    FolderPrivacy /*privacy*/) {
    // Sin permisos POSIX: la carpeta hereda la ACL del perfil del usuario.
    namespace fs = std::filesystem;
    try {
        const fs::path dir{path};
        if (!dir.parent_path().empty()) {
            std::error_code error;
            fs::create_directories(dir.parent_path(), error);
            if (error) {
                return "no se pudo crear " + dir.parent_path().string() + ": " + error.message();
            }
        }
        if (::CreateDirectoryW(utf8_to_utf16(path).c_str(), nullptr) == 0) {
            const DWORD error = ::GetLastError();
            if (error != ERROR_ALREADY_EXISTS) {
                return "no se pudo crear " + path + ": " + windows_error_text(error);
            }
        }
        return std::nullopt;
    } catch (const std::exception&) {
        // fs::path lanza si la ruta no es UTF-8 válido.
        return "no se pudo crear " + path + ": la ruta no es UTF-8 válido";
    }
}

std::optional<std::string> write_file_atomic(const std::string& path, std::string_view content,
                                             FilePrivacy /*file*/, FolderPrivacy folder) {
    namespace fs = std::filesystem;
    try {
        const fs::path dir = fs::path{path}.parent_path();
        if (!dir.empty()) {
            if (std::optional<std::string> error = create_private_directory(dir.string(), folder)) {
                return error;
            }
        }
    } catch (const std::exception&) {
        return "no se pudo escribir " + path + ": la ruta no es UTF-8 válido";
    }
    const std::string temporary = path + ".tmp";
    const std::wstring wide_temporary = utf8_to_utf16(temporary);
    const std::wstring wide_path = utf8_to_utf16(path);
    const HANDLE file = ::CreateFileW(wide_temporary.c_str(), GENERIC_WRITE, 0, nullptr,
                                      CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return "no se pudo escribir " + temporary + ": " + windows_error_text(::GetLastError());
    }
    bool ok = write_all(file, content) && ::FlushFileBuffers(file) != 0;
    DWORD failure = ok ? 0 : ::GetLastError();
    if (::CloseHandle(file) == 0 && ok) {
        ok = false;
        failure = ::GetLastError();
    }
    if (!ok) {
        ::DeleteFileW(wide_temporary.c_str());
        return "no se pudo escribir " + temporary + ": " + windows_error_text(failure);
    }
    // Un antivirus o el indexador pueden tener abierto el archivo un momento.
    DWORD error = 0;
    for (int attempt = 0;; ++attempt) {
        if (::MoveFileExW(wide_temporary.c_str(), wide_path.c_str(),
                          MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0) {
            return std::nullopt;
        }
        error = ::GetLastError();
        if ((error != ERROR_ACCESS_DENIED && error != ERROR_SHARING_VIOLATION) ||
            attempt == kMoveRetries) {
            break;
        }
        ::Sleep(kMoveRetryDelayMs);
    }
    ::DeleteFileW(wide_temporary.c_str());
    return "no se pudo reemplazar " + path + ": " + windows_error_text(error);
}

} // namespace chatbot
