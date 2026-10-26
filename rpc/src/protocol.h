#ifndef CHATBOT_RPC_PROTOCOL_H
#define CHATBOT_RPC_PROTOCOL_H

#include "conversation.h"

#include "chatbot/error.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>

namespace chatbot::rpc {

/// JSON del protocolo: ordered_json, para que cada mensaje salga con las
/// llaves en el orden de rpc/PROTOCOL.md (al cliente le da igual).
using Json = nlohmann::ordered_json;

/// Versión del protocolo (rpc/PROTOCOL.md, sección 11).
inline constexpr int kProtocolVersion = 1;

/// Códigos de error de JSON-RPC: los estándar y los de la aplicación
/// (rpc/PROTOCOL.md, sección 4).
enum class RpcError : int {
    ParseError = -32700,
    InvalidRequest = -32600,
    MethodNotFound = -32601,
    InvalidParams = -32602,
    InternalError = -32603,
    RequestCancelled = -32800,
    NotInitialized = -32001,
    Busy = -32002,
    UnknownConversation = -32003,
    NotConfigured = -32004,
    ValidationFailed = -32005,
    Unsupported = -32006,
    StorageError = -32007,
    ConfirmationRequired = -32008,
    ProtocolMismatch = -32010,
    StartupFailed = -32011,
};

/// Texto del campo message de cada código (en español).
[[nodiscard]] std::string_view error_message(RpcError code);

/// {"code", "message"} y "data" si no es null.
[[nodiscard]] Json error_object(RpcError code, const Json& data = nullptr);
/// data con el texto para el usuario: {"message": text}.
[[nodiscard]] Json message_data(std::string_view text);

/// Mensajes completos de JSON-RPC 2.0.
[[nodiscard]] Json response(const Json& id, const Json& result);
[[nodiscard]] Json error_response(const Json& id, RpcError code,
                                            const Json& data = nullptr);
[[nodiscard]] Json notification(std::string_view method, const Json& params);

/// Una línea del protocolo: UTF-8 inválido se sustituye en lugar de lanzar.
[[nodiscard]] std::string to_line(const Json& message);

/// ErrorKind en snake_case ("rate_limited").
[[nodiscard]] std::string_view error_kind_name(ErrorKind kind);
/// {"kind", "http_status", "message", "retry_after_seconds"}.
[[nodiscard]] Json encode_error(const ChatError& error);
/// "user" | "assistant" | "error" | "notice" | "sources".
[[nodiscard]] std::string_view entry_kind_name(cli::EntryKind kind);
/// Una entrada; "sources" solo en las de fuentes.
[[nodiscard]] Json encode_entry(const cli::Entry& entry);

/// Indicador de la respuesta en curso (rpc/PROTOCOL.md, sección 5).
enum class Phase { Idle, Searching, Thinking, Streaming };
[[nodiscard]] std::string_view phase_name(Phase phase);

/// Un parámetro de un método.
struct Param {
    enum class Type { String, Integer, Object, IntegerOrString };
    std::string_view name;
    Type type = Type::String;
    bool required = true;
};

/// Revisa params (por nombre): un objeto sin llaves de más, con las
/// requeridas y del tipo pedido. nullopt si está bien; si no, el data de
/// -32602: {"fields": [nombres]}. params ausente cuenta como {}.
[[nodiscard]] std::optional<Json> check_params(const Json* params,
                                                         std::initializer_list<Param> spec);

} // namespace chatbot::rpc

#endif // CHATBOT_RPC_PROTOCOL_H
