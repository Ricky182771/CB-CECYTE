#ifndef CHATBOT_CLI_DOWNLOADS_H
#define CHATBOT_CLI_DOWNLOADS_H

#include "clipboard.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace chatbot::cli {

/// Largo máximo, en bytes, de un nombre de archivo que da el usuario.
inline constexpr std::size_t kMaxFileNameBytes = 100;
/// Intentos con sufijo ("-2" a "-99") antes de rendirse si el nombre existe.
inline constexpr int kMaxNameSuffix = 99;

/// Carpeta de descargas, sin "chatbot/", en este orden:
/// 1. CHAT_DOWNLOAD_DIR, si está definida y no vacía (se usa aunque no exista);
/// 2. XDG_DOWNLOAD_DIR de $XDG_CONFIG_HOME/user-dirs.dirs (o
///    ~/.config/user-dirs.dirs), con $HOME expandido, si existe la carpeta;
/// 3. con TERMUX_VERSION, ~/storage/downloads si existe;
/// 4. ~/Descargas o ~/Downloads, la que exista;
/// 5. $HOME.
/// nullopt si no hay HOME ni CHAT_DOWNLOAD_DIR.
[[nodiscard]] std::optional<std::string> resolve_download_dir(const EnvLookup& env);

/// XDG_DOWNLOAD_DIR de un archivo user-dirs.dirs (líneas
/// XDG_DOWNLOAD_DIR="$HOME/Descargas" o con ruta absoluta), con $HOME
/// expandido a home. nullopt si no está, no se puede leer o no es válida.
[[nodiscard]] std::optional<std::string> read_xdg_download_dir(const std::string& path,
                                                               const std::string& home);

/// Extensión (con el punto) de un lenguaje de bloque de código, sin
/// distinguir mayúsculas: ".cpp", ".py"... ".txt" si no se conoce.
[[nodiscard]] std::string extension_for(std::string_view language);

/// Revisa un nombre que dio el usuario: solo el nombre base, sin "/", "\",
/// "..", caracteres de control, sin empezar con "." y de a lo más
/// kMaxFileNameBytes bytes de UTF-8 válido. Devuelve el motivo (para
/// mostrarlo) o nullopt si es válido.
[[nodiscard]] std::optional<std::string> validate_file_name(std::string_view name);

/// Nombre final del bloque: el del usuario (ya validado), con la extensión
/// del lenguaje si no tiene ninguna, o "bloque-<n><ext>" si está vacío.
[[nodiscard]] std::string block_file_name(std::string_view user_name, int number,
                                          std::string_view language);

/// text con un salto de línea al final si no lo tiene.
[[nodiscard]] std::string with_final_newline(std::string_view text);

/// Resultado de escribir un archivo: la ruta o el motivo del error.
struct WriteResult {
    std::string path;
    std::string error; ///< Vacío si salió bien.
};

/// Crea <base>/chatbot con 0755 (menos la umask) si no existe. Devuelve la
/// ruta o el error.
[[nodiscard]] WriteResult ensure_download_dir(const std::string& base);

/// Escribe content en dir/name sin sobrescribir nunca (O_CREAT | O_EXCL):
/// si el nombre existe, prueba "nombre-2.ext" hasta "-99". Permisos 0644
/// menos la umask (nunca ejecutable). Si falla a la mitad, borra lo que creó.
[[nodiscard]] WriteResult write_new_file(const std::string& dir, std::string_view name,
                                         std::string_view content);

/// Ruta para mostrar: con "~" en lugar de home al inicio.
[[nodiscard]] std::string display_path(const std::string& path, const std::string& home);

} // namespace chatbot::cli

#endif // CHATBOT_CLI_DOWNLOADS_H
