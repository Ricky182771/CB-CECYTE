#include "protocol.h"

#include <algorithm>
#include <vector>

namespace chatbot::rpc {

std::string_view error_message(RpcError code) {
    switch (code) {
    case RpcError::ParseError:
        return "La línea no es JSON.";
    case RpcError::InvalidRequest:
        return "No es una petición válida de JSON-RPC 2.0.";
    case RpcError::MethodNotFound:
        return "Método desconocido.";
    case RpcError::InvalidParams:
        return "Parámetros inválidos.";
    case RpcError::InternalError:
        return "Error interno del servidor.";
    case RpcError::RequestCancelled:
        return "La petición se canceló.";
    case RpcError::NotInitialized:
        return "Falta initialize.";
    case RpcError::Busy:
        return "Hay una respuesta en curso.";
    case RpcError::UnknownConversation:
        return "La conversación no existe o ya se cerró.";
    case RpcError::NotConfigured:
        return "Falta configurar el proveedor o el modelo.";
    case RpcError::ValidationFailed:
        return "Los datos no son válidos.";
    case RpcError::Unsupported:
        return "No está disponible en esta plataforma.";
    case RpcError::StorageError:
        return "No se pudo leer o escribir un archivo.";
    case RpcError::ConfirmationRequired:
        return "Hace falta la confirmación del usuario.";
    case RpcError::ProtocolMismatch:
        return "La versión del protocolo no coincide.";
    case RpcError::StartupFailed:
        return "El arranque falló.";
    }
    return "Error.";
}

Json error_object(RpcError code, const Json& data) {
    Json error{{"code", static_cast<int>(code)}, {"message", std::string{error_message(code)}}};
    if (!data.is_null()) {
        error["data"] = data;
    }
    return error;
}

Json message_data(std::string_view text) { return {{"message", std::string{text}}}; }

Json response(const Json& id, const Json& result) {
    return {{"jsonrpc", "2.0"}, {"id", id}, {"result", result}};
}

Json error_response(const Json& id, RpcError code,
                              const Json& data) {
    return {{"jsonrpc", "2.0"}, {"id", id}, {"error", error_object(code, data)}};
}

Json notification(std::string_view method, const Json& params) {
    return {{"jsonrpc", "2.0"}, {"method", std::string{method}}, {"params", params}};
}

std::string to_line(const Json& message) {
    return message.dump(-1, ' ', false, Json::error_handler_t::replace) + '\n';
}

std::string_view error_kind_name(ErrorKind kind) {
    switch (kind) {
    case ErrorKind::Config:
        return "config";
    case ErrorKind::Auth:
        return "auth";
    case ErrorKind::ModelNotFound:
        return "model_not_found";
    case ErrorKind::RateLimited:
        return "rate_limited";
    case ErrorKind::Server:
        return "server";
    case ErrorKind::Network:
        return "network";
    case ErrorKind::Timeout:
        return "timeout";
    case ErrorKind::BadResponse:
        return "bad_response";
    case ErrorKind::InvalidRequest:
        return "invalid_request";
    case ErrorKind::Cancelled:
        return "cancelled";
    case ErrorKind::NoSearchResults:
        return "no_search_results";
    }
    return "bad_response";
}

Json encode_error(const ChatError& error) {
    return {{"kind", std::string{error_kind_name(error.kind)}},
            {"http_status", error.http_status},
            {"message", error.message},
            {"retry_after_seconds", error.retry_after.has_value()
                                        ? Json(error.retry_after->count())
                                        : Json(nullptr)}};
}

std::string_view entry_kind_name(cli::EntryKind kind) {
    switch (kind) {
    case cli::EntryKind::User:
        return "user";
    case cli::EntryKind::Assistant:
        return "assistant";
    case cli::EntryKind::Error:
        return "error";
    case cli::EntryKind::Notice:
        return "notice";
    case cli::EntryKind::Sources:
        return "sources";
    }
    return "notice";
}

Json encode_entry(const cli::Entry& entry) {
    Json json{{"kind", std::string{entry_kind_name(entry.kind)}},
                        {"text", entry.text},
                        {"in_progress", entry.in_progress},
                        {"incomplete", entry.incomplete},
                        {"cancelled", entry.cancelled},
                        {"note", entry.note}};
    if (entry.kind == cli::EntryKind::Sources) {
        Json sources = Json::array();
        int n = 0;
        for (const SearchResult& source : entry.sources) {
            sources.push_back({{"n", ++n},
                               {"title", source.title},
                               {"url", source.url},
                               {"published_date", source.published_date.empty()
                                                      ? Json(nullptr)
                                                      : Json(source.published_date)}});
        }
        json["sources"] = std::move(sources);
    }
    return json;
}

std::string_view phase_name(Phase phase) {
    switch (phase) {
    case Phase::Idle:
        return "idle";
    case Phase::Searching:
        return "searching";
    case Phase::Thinking:
        return "thinking";
    case Phase::Streaming:
        return "streaming";
    }
    return "idle";
}

std::optional<Json> check_params(const Json* params,
                                           std::initializer_list<Param> spec) {
    static const Json empty = Json::object();
    if (params == nullptr) {
        params = &empty;
    }
    if (!params->is_object()) {
        return Json{{"fields", Json::array()}};
    }
    std::vector<std::string> fields;
    for (const auto& [key, value] : params->items()) {
        if (std::none_of(spec.begin(), spec.end(),
                         [&key](const Param& param) { return param.name == key; })) {
            fields.push_back(key);
        }
    }
    for (const Param& param : spec) {
        const auto found = params->find(std::string{param.name});
        if (found == params->end()) {
            if (param.required) {
                fields.emplace_back(param.name);
            }
            continue;
        }
        bool ok = false;
        switch (param.type) {
        case Param::Type::String:
            ok = found->is_string();
            break;
        case Param::Type::Integer:
            ok = found->is_number_integer();
            break;
        case Param::Type::Boolean:
            ok = found->is_boolean();
            break;
        case Param::Type::Object:
            ok = found->is_object();
            break;
        case Param::Type::IntegerOrString:
            ok = found->is_number_integer() || found->is_string();
            break;
        case Param::Type::Any: // Lo revisa el método.
            ok = true;
            break;
        }
        if (!ok) {
            fields.emplace_back(param.name);
        }
    }
    if (fields.empty()) {
        return std::nullopt;
    }
    return Json{{"fields", fields}};
}

} // namespace chatbot::rpc
