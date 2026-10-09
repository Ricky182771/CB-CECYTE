#ifndef CHATBOT_PLATFORM_H
#define CHATBOT_PLATFORM_H

#include <ctime>
#include <optional>
#include <string>
#include <string_view>

/// Capa de plataforma: lo único que cambia entre POSIX y Windows. CMake
/// compila platform_posix.cpp o platform_windows.cpp; el resto del código no
/// lleva #ifdef. Toda cadena es UTF-8 en las dos plataformas (en Windows, el
/// manifiesto pone UTF-8 como página de códigos ANSI, y las llamadas a Win32
/// usan la versión W con conversión explícita).
namespace chatbot {

/// Sistema operativo. Las funciones puras cuyas reglas cambian según el
/// sistema (rutas, portapapeles) lo reciben como parámetro, para poder probar
/// las de Windows también en Linux.
enum class Os { Posix, Windows };

/// El sistema para el que se compiló este ejecutable.
[[nodiscard]] Os current_os();

/// Hora local (localtime_r en POSIX, localtime_s en Windows); nullopt si no
/// se puede convertir.
[[nodiscard]] std::optional<std::tm> local_time(std::time_t time);

/// true si stdin y stdout son una terminal. En Windows, si los dos son una
/// consola (GetConsoleMode); no se usa _isatty, que también da verdadero con
/// NUL. mintty (la terminal de MSYS2) no es una consola: da false.
[[nodiscard]] bool stdio_is_terminal();

/// Como stdio_is_terminal, pero solo stdout.
[[nodiscard]] bool stdout_is_terminal();

/// Permisos del archivo que deja write_file_atomic. Solo aplican en POSIX: en
/// Windows el archivo hereda la ACL de su carpeta (la del perfil del usuario).
enum class FilePrivacy {
    Private,      ///< 0600, aunque el archivo ya existiera con otros.
    KeepExisting, ///< Los permisos que ya tenía; 0644 si no existía.
};

/// Qué hacer con una carpeta privada (create_private_directory). Solo aplica
/// en POSIX: en Windows la carpeta hereda la ACL del perfil.
enum class FolderPrivacy {
    Private,      ///< 0700 al crearla y, si ya existía, se deja en 0700.
    PrivateIfNew, ///< 0700 al crearla; si ya existía, no se toca.
};

/// Crea la carpeta (y sus padres, con los permisos por defecto) si falta.
/// POSIX: mkdir 0700 y, con FolderPrivacy::Private, chmod 0700. Windows:
/// CreateDirectoryW. Devuelve el error en español, o nullopt.
[[nodiscard]] std::optional<std::string> create_private_directory(const std::string& path,
                                                                  FolderPrivacy privacy);

/// Escribe content en path de forma atómica, a través de <path>.tmp, y crea
/// su carpeta con create_private_directory(folder) si falta.
/// - POSIX: .tmp con los permisos de file (fchmod: la umask no los cambia),
///   fsync, rename y fsync de la carpeta.
/// - Windows: .tmp con CreateFileW, WriteFile, FlushFileBuffers y
///   MoveFileExW(MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH); si el
///   reemplazo falla con acceso denegado o archivo en uso (un antivirus o el
///   indexador), lo reintenta hasta 5 veces cada 50 ms.
/// Si falla, borra el .tmp. Devuelve el error en español, o nullopt.
[[nodiscard]] std::optional<std::string> write_file_atomic(const std::string& path,
                                                           std::string_view content,
                                                           FilePrivacy file,
                                                           FolderPrivacy folder);

} // namespace chatbot

#endif // CHATBOT_PLATFORM_H
