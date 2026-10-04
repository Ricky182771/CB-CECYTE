#ifndef CHATBOT_ATOMIC_FILE_H
#define CHATBOT_ATOMIC_FILE_H

#include <sys/types.h>

#include <optional>
#include <string>

namespace chatbot {

/// Escribe content en path de forma atómica: <path>.tmp con file_mode +
/// fsync + rename + fsync del directorio. Crea la carpeta si falta y la deja
/// en dir_mode. Devuelve el error en español, o nullopt. Header interno.
[[nodiscard]] std::optional<std::string> write_file_atomic(const std::string& path,
                                                           const std::string& content,
                                                           mode_t file_mode, mode_t dir_mode);

} // namespace chatbot

#endif // CHATBOT_ATOMIC_FILE_H
