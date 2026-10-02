#ifndef CHATBOT_TYPES_H
#define CHATBOT_TYPES_H

#include <string>

namespace chatbot {

/// Rol de un mensaje en la conversación.
enum class Role {
    System,
    User,
    Assistant,
};

/// Un mensaje de la conversación. El historial lo administra quien llama;
/// el núcleo no lo guarda (hito 1).
struct Message {
    Role role;
    std::string content;
};

} // namespace chatbot

#endif // CHATBOT_TYPES_H
