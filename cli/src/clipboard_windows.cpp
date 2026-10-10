// Portapapeles en Windows: la API de Win32 (CF_UNICODETEXT), sin lanzar
// programas. CMake lo compila en lugar de clipboard_posix.cpp en Windows.

#include "clipboard.h"
#include "paste.h"

#include "chatbot/platform_windows.h"

#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string>

namespace chatbot::cli {

namespace {

/// Intentos de OpenClipboard si otro proceso tiene el portapapeles abierto.
constexpr int kOpenAttempts = 10;
constexpr DWORD kOpenRetryDelayMs = 20;

/// Ventana de solo mensajes (HWND_MESSAGE) para ser dueña del portapapeles
/// durante la operación: con OpenClipboard(NULL), EmptyClipboard deja el
/// dueño nulo y SetClipboardData puede fallar.
class MessageWindow {
public:
    MessageWindow()
        : handle_(::CreateWindowExW(0, L"Message", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                    ::GetModuleHandleW(nullptr), nullptr)) {}
    ~MessageWindow() {
        if (handle_ != nullptr) {
            ::DestroyWindow(handle_);
        }
    }
    MessageWindow(const MessageWindow&) = delete;
    MessageWindow& operator=(const MessageWindow&) = delete;
    [[nodiscard]] HWND get() const { return handle_; }

private:
    HWND handle_;
};

/// El portapapeles abierto mientras vive (CloseClipboard al salir).
class OpenedClipboard {
public:
    explicit OpenedClipboard(HWND owner) {
        for (int attempt = 0; attempt < kOpenAttempts; ++attempt) {
            if (::OpenClipboard(owner) != 0) {
                open_ = true;
                return;
            }
            ::Sleep(kOpenRetryDelayMs);
        }
    }
    ~OpenedClipboard() {
        if (open_) {
            ::CloseClipboard();
        }
    }
    OpenedClipboard(const OpenedClipboard&) = delete;
    OpenedClipboard& operator=(const OpenedClipboard&) = delete;
    [[nodiscard]] bool is_open() const { return open_; }

private:
    bool open_ = false;
};

/// Memoria de GlobalAlloc que se libera sola, salvo que pase al sistema.
class GlobalMemory {
public:
    explicit GlobalMemory(SIZE_T bytes) : handle_(::GlobalAlloc(GMEM_MOVEABLE, bytes)) {}
    ~GlobalMemory() {
        if (handle_ != nullptr) {
            ::GlobalFree(handle_);
        }
    }
    GlobalMemory(const GlobalMemory&) = delete;
    GlobalMemory& operator=(const GlobalMemory&) = delete;
    [[nodiscard]] HGLOBAL get() const { return handle_; }
    /// Después de SetClipboardData la memoria es del sistema.
    void release() { handle_ = nullptr; }

private:
    HGLOBAL handle_;
};

/// GlobalLock mientras vive (GlobalUnlock al salir).
class GlobalLockGuard {
public:
    explicit GlobalLockGuard(HGLOBAL memory)
        : memory_(memory), data_(::GlobalLock(memory)) {}
    ~GlobalLockGuard() {
        if (data_ != nullptr) {
            ::GlobalUnlock(memory_);
        }
    }
    GlobalLockGuard(const GlobalLockGuard&) = delete;
    GlobalLockGuard& operator=(const GlobalLockGuard&) = delete;
    [[nodiscard]] const void* data() const { return data_; }

private:
    HGLOBAL memory_;
    void* data_;
};

} // namespace

bool find_in_path(std::string_view /*program*/) {
    return false; // En Windows no se lanzan programas de portapapeles.
}

bool run_with_input(const std::vector<std::string>& /*argv*/, std::string_view /*input*/,
                    std::chrono::milliseconds /*timeout*/) {
    return false; // Solo POSIX: clipboard_methods nunca propone programas en Windows.
}

bool copy_to_native_clipboard(std::string_view text) {
    const std::wstring wide = chatbot::utf8_to_utf16(to_crlf(text));
    if (wide.empty() && !text.empty()) {
        return false; // No se pudo convertir.
    }
    const MessageWindow window;
    if (window.get() == nullptr) {
        return false;
    }
    const OpenedClipboard clipboard(window.get());
    if (!clipboard.is_open() || ::EmptyClipboard() == 0) {
        return false;
    }
    const SIZE_T bytes = (wide.size() + 1) * sizeof(wchar_t); // Con el L'\0' final.
    GlobalMemory memory(bytes);
    if (memory.get() == nullptr) {
        return false;
    }
    void* locked = ::GlobalLock(memory.get());
    if (locked == nullptr) {
        return false;
    }
    std::memcpy(locked, wide.c_str(), bytes);
    ::GlobalUnlock(memory.get());
    if (::SetClipboardData(CF_UNICODETEXT, memory.get()) == nullptr) {
        return false;
    }
    memory.release();
    return true;
}

std::optional<std::string> read_native_clipboard() {
    if (::IsClipboardFormatAvailable(CF_UNICODETEXT) == 0) {
        return std::nullopt;
    }
    // Para leer no hace falta ser dueño: OpenClipboard(nullptr) basta.
    const OpenedClipboard clipboard(nullptr);
    if (!clipboard.is_open()) {
        return std::nullopt;
    }
    const HANDLE data = ::GetClipboardData(CF_UNICODETEXT);
    if (data == nullptr) {
        return std::nullopt;
    }
    const SIZE_T bytes = ::GlobalSize(data);
    const GlobalLockGuard lock(data);
    if (lock.data() == nullptr) {
        return std::nullopt;
    }
    // Sin confiar en el L'\0' final: a lo más lo que mide el bloque. Cada
    // unidad UTF-16 da al menos un byte de UTF-8, así que con
    // kMaxPasteBytes + 1 unidades sanitize_paste todavía sabe que recortó.
    const auto* text = static_cast<const wchar_t*>(lock.data());
    const std::size_t limit =
        std::min<std::size_t>(bytes / sizeof(wchar_t), kMaxPasteBytes + 1);
    std::size_t length = 0;
    while (length < limit && text[length] != L'\0') {
        ++length;
    }
    const std::wstring wide(text, length);
    return chatbot::utf16_to_utf8(wide);
}

} // namespace chatbot::cli
