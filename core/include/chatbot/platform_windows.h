#ifndef CHATBOT_PLATFORM_WINDOWS_H
#define CHATBOT_PLATFORM_WINDOWS_H

#include <string>
#include <string_view>

/// Ayudas de texto para llamar a Win32. Solo existen en Windows (las define
/// platform_windows.cpp): solo las incluyen los .cpp de Windows. Las llamadas
/// a Win32 usan siempre la versión W con estas conversiones, así no dependen
/// de la página de códigos del proceso.
namespace chatbot {

/// UTF-8 → UTF-16 (MultiByteToWideChar). Los bytes inválidos quedan como
/// U+FFFD.
[[nodiscard]] std::wstring utf8_to_utf16(std::string_view text);

/// UTF-16 → UTF-8 (WideCharToMultiByte). Los sustitutos sueltos quedan como
/// U+FFFD.
[[nodiscard]] std::string utf16_to_utf8(std::wstring_view text);

/// Texto del sistema para un código de GetLastError (FormatMessageW, en el
/// idioma de Windows), en UTF-8 y con el código: "Acceso denegado (código 5)".
[[nodiscard]] std::string windows_error_text(unsigned long code);

} // namespace chatbot

#endif // CHATBOT_PLATFORM_WINDOWS_H
