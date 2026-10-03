#ifndef CHATBOT_HISTORY_H
#define CHATBOT_HISTORY_H

#include "chatbot/types.h"

#include <cstddef>
#include <vector>

namespace chatbot {

/// Resultado de trim_history: el historial recortado y cuántos mensajes se
/// quitaron.
struct TrimResult {
    std::vector<Message> messages;
    std::size_t dropped = 0;
};

/// Recorta el historial para que el total de bytes UTF-8 de los content
/// (aproximadamente caracteres) no pase de limit_bytes. Función pura: el
/// núcleo no guarda el historial (sección 6).
/// - Los mensajes System iniciales y el último mensaje nunca se quitan.
/// - Se quitan los más viejos después del bloque de sistema, en pares
///   usuario+asistente, para que tras el sistema lo primero siga siendo User.
/// - Se para al caber, o cuando solo quedan el sistema y el último mensaje
///   (aunque sigan excediendo).
/// - limit_bytes == 0 devuelve todo sin cambios.
/// No lanza excepciones (salvo falta de memoria al copiar).
[[nodiscard]] TrimResult trim_history(const std::vector<Message>& messages,
                                      std::size_t limit_bytes);

} // namespace chatbot

#endif // CHATBOT_HISTORY_H
