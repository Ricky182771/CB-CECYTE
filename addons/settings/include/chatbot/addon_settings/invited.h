#ifndef CHATBOT_ADDON_SETTINGS_INVITED_H
#define CHATBOT_ADDON_SETTINGS_INVITED_H

#include "chatbot/result.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

/// invitados.json ({"version": 1, "users": [IDs]}): quienes entraron con el
/// código de invitación. Lo escriben dos procesos (el bot al aceptar un
/// código y la app al quitar a alguien), así que todo pasa por un flock
/// sobre <archivo>.lock y nadie guarda una copia en memoria como fuente de
/// verdad: add_invited y remove_invited leen, cambian y escriben bajo el
/// candado y devuelven la lista que quedó. Escritura atómica
/// (write_file_atomic) con 600; la carpeta, 700 si se crea. Si el archivo no
/// se entiende, error BadResponse con la ruta y el motivo, y no se toca (el
/// bot sale con 65); lo demás (leer, el candado, escribir) da Config. En
/// Windows, todo da "no disponible".
namespace chatbot::addon_settings {

/// La lista guardada (vacía si el archivo o su carpeta no existen).
[[nodiscard]] Result<std::vector<std::int64_t>> load_invited(const std::string& path);

/// Agrega user (si no estaba) y devuelve la lista completa, en orden.
[[nodiscard]] Result<std::vector<std::int64_t>> add_invited(const std::string& path,
                                                            std::int64_t user);

/// Quita user (si estaba) y devuelve la lista que quedó.
[[nodiscard]] Result<std::vector<std::int64_t>> remove_invited(const std::string& path,
                                                               std::int64_t user);

namespace detail {

/// El contenido de invitados.json (función pura). Error BadResponse que dice
/// qué está mal y que el archivo no se tocó.
[[nodiscard]] Result<std::vector<std::int64_t>> parse_invited(std::string_view path,
                                                              std::string_view text);
/// El texto que se escribe.
[[nodiscard]] std::string format_invited(const std::vector<std::int64_t>& users);

} // namespace detail

} // namespace chatbot::addon_settings

#endif // CHATBOT_ADDON_SETTINGS_INVITED_H
