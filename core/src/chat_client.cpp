#include "chatbot/chat_client.h"

#include "chatbot/error.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace chatbot {
namespace {

using nlohmann::json;

/// Máximo de reintentos: 1 petición original + 3 reintentos (decisión del
/// usuario sobre la sección 6).
constexpr int kMaxRetries = 3;

/// Espera base del backoff exponencial: 1 s, 2 s, 4 s (sección 6).
constexpr std::chrono::seconds kBaseDelay{1};

/// Tope de espera entre intentos, también aplicado a Retry-After (decisión
/// del usuario para evitar esperas absurdas).
constexpr std::chrono::seconds kMaxDelay{60};

/// Longitud máxima del mensaje extraído del cuerpo de un error.
constexpr std::size_t kMaxErrorMessageLength = 300;

/// Corta textos demasiado largos (sección 8).
std::string truncate_message(const std::string& text) {
    if (text.size() <= kMaxErrorMessageLength) {
        return text;
    }
    return text.substr(0, kMaxErrorMessageLength) + "...";
}

/// Convierte un rol a su cadena del protocolo OpenAI. Valida el valor.
Result<std::string_view> role_to_string(Role role) {
    switch (role) {
    case Role::System:
        return std::string_view{"system"};
    case Role::User:
        return std::string_view{"user"};
    case Role::Assistant:
        return std::string_view{"assistant"};
    }
    return ChatError{ErrorKind::Config, 0,
                     "Rol de mensaje inválido en el historial.", std::nullopt};
}

/// Construye el cuerpo JSON de la petición (sección 8).
Result<std::string> build_request_body(std::string_view model,
                                       const std::vector<Message>& messages) {
    if (messages.empty()) {
        return ChatError{ErrorKind::Config, 0,
                         "La conversación no puede estar vacía.", std::nullopt};
    }

    json body;
    body["model"] = model;
    body["stream"] = false;
    json json_messages = json::array();
    for (const Message& message : messages) {
        const Result<std::string_view> role = role_to_string(message.role);
        if (role.is_error()) {
            return Result<std::string>{role.error()};
        }
        if (message.content.empty()) {
            return ChatError{ErrorKind::Config, 0,
                             "Hay un mensaje sin contenido en el historial.", std::nullopt};
        }
        json_messages.push_back(json{{"role", role.value()},
                                     {"content", message.content}});
    }
    body["messages"] = std::move(json_messages);
    return body.dump();
}

/// URL final del endpoint: base sin "/" final + "/chat/completions".
std::string build_url(std::string_view base_url) {
    std::string base{base_url};
    while (!base.empty() && base.back() == '/') {
        base.pop_back();
    }
    return base + "/chat/completions";
}

/// Intenta extraer el mensaje de error del cuerpo (error.message o detail,
/// sección 8); si no hay, usa el texto alternativo.
std::string extract_error_message(const std::string& body, const std::string& fallback) {
    try {
        const json document = json::parse(body);
        if (!document.is_object()) {
            return fallback;
        }
        if (document.contains("error")) {
            const json& error = document.at("error");
            if (error.is_object() && error.contains("message") &&
                error.at("message").is_string()) {
                return truncate_message(error.at("message").get<std::string>());
            }
        }
        if (document.contains("detail") && document.at("detail").is_string()) {
            return truncate_message(document.at("detail").get<std::string>());
        }
    } catch (const json::exception&) {
        // Cuerpo no interpretable: se usa el texto alternativo.
    }
    return fallback;
}

/// Interpreta el valor crudo de Retry-After como segundos enteros, con el
/// tope decidido. Nullopt si no es un entero válido.
std::optional<std::chrono::seconds> parse_retry_after(const std::optional<std::string>& raw) {
    if (!raw.has_value() || raw->empty()) {
        return std::nullopt;
    }
    long long seconds = 0;
    const char* begin = raw->data();
    const char* end = begin + raw->size();
    const std::from_chars_result result = std::from_chars(begin, end, seconds);
    if (result.ec != std::errc{} || result.ptr != end || seconds < 0) {
        return std::nullopt;
    }
    return std::chrono::seconds{std::min<long long>(seconds, kMaxDelay.count())};
}

/// Mapea un estado HTTP distinto de 2xx a un ChatError (sección 8).
ChatError map_http_error(int status, const std::string& body) {
    ChatError error;
    error.http_status = status;
    error.message = extract_error_message(
        body, "El servidor respondió con el estado HTTP " + std::to_string(status) + ".");

    switch (status) {
    case 401:
    case 403:
        error.kind = ErrorKind::Auth;
        break;
    case 404:
    case 410:
        error.kind = ErrorKind::ModelNotFound;
        break;
    case 429:
        error.kind = ErrorKind::RateLimited;
        break;
    default:
        error.kind = status >= 500 && status <= 599 ? ErrorKind::Server : ErrorKind::BadResponse;
        break;
    }

    return error;
}

/// Variante de map_http_error que ya conoce el valor crudo de Retry-After.
ChatError map_http_error(int status, const std::string& body,
                         const std::optional<std::string>& retry_after_raw) {
    ChatError error = map_http_error(status, body);
    if (error.kind == ErrorKind::RateLimited) {
        error.retry_after = parse_retry_after(retry_after_raw);
    }
    return error;
}

/// Interpreta una respuesta 2xx (sección 8). Cualquier campo ausente o con
/// tipo inesperado produce BadResponse; nunca se accede a índices inexistentes.
Result<std::string> parse_success_response(const std::string& body) {
    try {
        const json document = json::parse(body);
        if (!document.is_object()) {
            return ChatError{ErrorKind::BadResponse, 0,
                             "La respuesta no es un objeto JSON.", std::nullopt};
        }
        const auto choices_it = document.find("choices");
        if (choices_it == document.end() || !choices_it->is_array()) {
            return ChatError{ErrorKind::BadResponse, 0,
                             "La respuesta no contiene la lista \"choices\".", std::nullopt};
        }
        if (choices_it->empty()) {
            return ChatError{ErrorKind::BadResponse, 0,
                             "La lista \"choices\" está vacía.", std::nullopt};
        }
        const json& first = choices_it->front();
        if (!first.is_object() || !first.contains("message")) {
            return ChatError{ErrorKind::BadResponse, 0,
                             "\"choices[0]\" no contiene \"message\".", std::nullopt};
        }
        const json& message = first.at("message");
        if (!message.is_object() || !message.contains("content")) {
            return ChatError{ErrorKind::BadResponse, 0,
                             "\"choices[0].message\" no contiene \"content\".", std::nullopt};
        }
        const json& content = message.at("content");
        if (!content.is_string()) {
            return ChatError{ErrorKind::BadResponse, 0,
                             "\"choices[0].message.content\" no es una cadena.", std::nullopt};
        }
        return content.get<std::string>();
    } catch (const json::exception& e) {
        return ChatError{ErrorKind::BadResponse, 0,
                         "JSON malformado en la respuesta: " + std::string{e.what()},
                         std::nullopt};
    }
}

/// Indica si un tipo de error se reintenta (sección 6).
bool is_retryable(ErrorKind kind) {
    switch (kind) {
    case ErrorKind::RateLimited:
    case ErrorKind::Server:
    case ErrorKind::Network:
    case ErrorKind::Timeout:
        return true;
    case ErrorKind::Auth:
    case ErrorKind::ModelNotFound:
    case ErrorKind::BadResponse:
    case ErrorKind::Config:
    case ErrorKind::Cancelled:
        return false;
    }
    return false;
}

} // namespace

ChatClient::ChatClient(Config config, std::unique_ptr<Transport> transport)
    : ChatClient(std::move(config), std::move(transport), std::make_unique<RealSleeper>()) {}

ChatClient::ChatClient(Config config, std::unique_ptr<Transport> transport,
                       std::unique_ptr<Sleeper> sleeper)
    : config_(std::move(config)), transport_(std::move(transport)),
      sleeper_(std::move(sleeper)) {}

ChatClient::~ChatClient() = default;

Sleeper& ChatClient::sleeper() { return *sleeper_; }

Result<std::string> ChatClient::complete(const std::vector<Message>& messages) {
    // Revalida la configuración: el cliente puede construirse con un Config
    // armado a mano (decisión del usuario: validar en ambos lados).
    if (const std::optional<ChatError> config_error = validate_config(config_)) {
        return *config_error;
    }

    const Result<std::string> body = build_request_body(config_.model, messages);
    if (body.is_error()) {
        return Result<std::string>{body.error()};
    }

    HttpRequest request;
    request.url = build_url(config_.base_url);
    request.body = body.value();
    request.api_key = config_.api_key;
    request.timeout = std::chrono::milliseconds(config_.timeout_seconds);

    for (int attempt = 0;; ++attempt) {
        const HttpResponse response = transport_->send(request);

        // Sin respuesta HTTP: fallo de red o timeout de curl.
        if (response.status == 0) {
            ChatError error;
            error.kind = response.timed_out ? ErrorKind::Timeout : ErrorKind::Network;
            error.message = !response.error.empty()
                                ? response.error
                                : std::string{error_kind_label(error.kind)};
            if (attempt < kMaxRetries && is_retryable(error.kind)) {
                sleeper().sleep_for(kBaseDelay * (1 << attempt));
                continue;
            }
            return error;
        }

        // 2xx: interpretar el cuerpo como respuesta exitosa.
        if (response.status >= 200 && response.status <= 299) {
            return parse_success_response(response.body);
        }

        // Error HTTP: mapear y decidir si se reintenta.
        ChatError error = map_http_error(response.status, response.body, response.retry_after);
        if (attempt < kMaxRetries && is_retryable(error.kind)) {
            std::chrono::milliseconds wait{kBaseDelay * (1 << attempt)};
            if (error.retry_after.has_value()) {
                wait = *error.retry_after; // Ya viene acotada a kMaxDelay.
            }
            sleeper().sleep_for(wait);
            continue;
        }
        return error;
    }
}

} // namespace chatbot
