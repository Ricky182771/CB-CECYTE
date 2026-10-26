#ifndef CHATBOT_RPC_DEBUG_LOG_H
#define CHATBOT_RPC_DEBUG_LOG_H

#include "protocol.h"

#include <fstream>
#include <string>
#include <string_view>

namespace chatbot::rpc {

/// Valor con el que se tapan los secretos.
inline constexpr std::string_view kRedacted = "•••";

/// message con el valor de api_key, token, model_key y search_key tapado,
/// a cualquier profundidad (en objetos y arreglos).
[[nodiscard]] Json redact_secrets(Json message);

/// CHAT_DEBUG_RPC: cada línea que entra ("→ ") y sale ("← "), con los
/// secretos tapados antes de escribir. Una línea que no es JSON no se puede
/// tapar: solo se anota su tamaño. Contiene la conversación: solo para
/// diagnosticar. Si no se puede abrir o escribir, se ignora en silencio.
class DebugLog {
public:
    /// path vacío: no escribe nada.
    explicit DebugLog(const std::string& path);

    void incoming(std::string_view line);
    void outgoing(const Json& message);
    /// Algo que no es una línea completa (una línea de más de 1 MiB).
    void note(std::string_view text);

private:
    void write(std::string_view prefix, std::string_view text);

    std::ofstream file_;
};

} // namespace chatbot::rpc

#endif // CHATBOT_RPC_DEBUG_LOG_H
