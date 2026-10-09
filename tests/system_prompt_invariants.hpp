#ifndef CHATBOT_TEST_SYSTEM_PROMPT_INVARIANTS_HPP
#define CHATBOT_TEST_SYSTEM_PROMPT_INVARIANTS_HPP

#include "chatbot/transport.h"
#include "chatbot/types.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace chatbot_test {

/// Cómo debe terminar la lista de mensajes que se revisa.
enum class Ending {
    /// Lo que se manda al servidor (o el historial con una petición en
    /// curso): el último mensaje es el User que se acaba de enviar (I3).
    Sent,
    /// El historial sin petición en curso: nunca termina en un User huérfano
    /// (tras un error, una cancelación o una búsqueda sin resultados).
    Idle,
};

/// Revisa los invariantes del mensaje de sistema con el prompt efectivo
/// prompt (vacío = sin mensaje de sistema):
/// - I1: si prompt no está vacío, el primer mensaje es {system, prompt},
///   idéntico byte por byte;
/// - I2: exactamente un System si prompt no está vacío; ninguno si lo está;
/// - I3 (Ending::Sent): el último mensaje es User y, si se da last_user, su
///   content es exactamente ese;
/// - I4: después del sistema, lo primero es User y los roles se alternan.
/// Con Ending::Idle, en lugar de I3: el último mensaje no es User.
/// Devuelve la primera violación encontrada, o "" si se cumplen todos. Así
/// las pruebas con miles de casos hacen una sola aserción por caso y pueden
/// imprimir la semilla y el índice con INFO.
[[nodiscard]] inline std::string check_invariants(
    const std::vector<chatbot::Message>& messages, std::string_view prompt,
    Ending ending = Ending::Sent, std::optional<std::string_view> last_user = std::nullopt) {
    using chatbot::Role;
    std::size_t begin = 0;
    if (!prompt.empty()) {
        if (messages.empty() || messages.front().role != Role::System) {
            return "I1: el primer mensaje no es System";
        }
        if (messages.front().content != prompt) {
            return "I1: el content del System (" + std::to_string(messages.front().content.size()) +
                   " B) no es el prompt efectivo (" + std::to_string(prompt.size()) + " B)";
        }
        begin = 1;
    }
    for (std::size_t i = begin; i < messages.size(); ++i) {
        if (messages[i].role == Role::System) {
            return "I2: mensaje System de más en la posición " + std::to_string(i);
        }
    }
    for (std::size_t i = begin; i < messages.size(); ++i) {
        // Tras el sistema: User, Assistant, User, ...
        const Role expected = (i - begin) % 2 == 0 ? Role::User : Role::Assistant;
        if (messages[i].role != expected) {
            return "I4: rol fuera de la alternancia en la posición " + std::to_string(i);
        }
    }
    const bool ends_in_user = messages.size() > begin && messages.back().role == Role::User;
    if (ending == Ending::Sent) {
        if (!ends_in_user) {
            return "I3: el último mensaje no es User";
        }
        if (last_user.has_value() && messages.back().content != *last_user) {
            return "I3: el último User no es el que se acaba de enviar";
        }
    } else if (ends_in_user) {
        return "User huérfano al final del historial";
    }
    return {};
}

/// Los messages del cuerpo JSON de una petición, como los ve el modelo. Un
/// rol desconocido o un content que no sea cadena lanza una excepción (la
/// prueba falla): no debe pasar nunca.
[[nodiscard]] inline std::vector<chatbot::Message> sent_messages_of(
    const chatbot::HttpRequest& request) {
    // El documento va en una variable: en un for sobre parse(...).at(...)
    // el temporal de parse se destruiría antes del ciclo.
    const nlohmann::json body = nlohmann::json::parse(request.body);
    std::vector<chatbot::Message> messages;
    for (const nlohmann::json& item : body.at("messages")) {
        const std::string role = item.at("role").get<std::string>();
        chatbot::Message message;
        if (role == "system") {
            message.role = chatbot::Role::System;
        } else if (role == "user") {
            message.role = chatbot::Role::User;
        } else if (role == "assistant") {
            message.role = chatbot::Role::Assistant;
        } else {
            throw std::runtime_error{"rol desconocido en el cuerpo: " + role};
        }
        message.content = item.at("content").get<std::string>();
        messages.push_back(std::move(message));
    }
    return messages;
}

/// Total de bytes UTF-8 de los content, como los mide trim_history.
[[nodiscard]] inline std::size_t content_bytes(const std::vector<chatbot::Message>& messages) {
    std::size_t total = 0;
    for (const chatbot::Message& message : messages) {
        total += message.content.size();
    }
    return total;
}

} // namespace chatbot_test

#endif // CHATBOT_TEST_SYSTEM_PROMPT_INVARIANTS_HPP
