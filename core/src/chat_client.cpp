#include "chatbot/chat_client.h"

#include "chatbot/cancel_token.h"
#include "chatbot/error.h"
#include "error_body.h"
#include "sse.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <ctime>
#include <exception>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
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

/// Construye el cuerpo JSON de la petición (sección 8). stream indica si se
/// pide respuesta por fragmentos (hito 2) o completa (hito 1).
Result<std::string> build_request_body(std::string_view model,
                                       const std::vector<Message>& messages,
                                       bool stream) {
    if (messages.empty()) {
        return ChatError{ErrorKind::Config, 0,
                         "La conversación no puede estar vacía.", std::nullopt};
    }

    json body;
    body["model"] = model;
    body["stream"] = stream;
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
    // replace: los bytes UTF-8 inválidos se sustituyen por U+FFFD en lugar de
    // lanzar type_error 316 (las excepciones no cruzan la API pública).
    return body.dump(-1, ' ', false, json::error_handler_t::replace);
}

/// URL final del endpoint: base sin "/" final + "/chat/completions".
std::string build_url(std::string_view base_url) {
    std::string base{base_url};
    while (!base.empty() && base.back() == '/') {
        base.pop_back();
    }
    return base + "/chat/completions";
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
    case ErrorKind::InvalidRequest:
    case ErrorKind::Config:
    case ErrorKind::Cancelled:
        return false;
    }
    return false;
}

/// Mapea un estado HTTP distinto de 2xx a un ChatError (sección 8).
ChatError map_http_error(int status, const std::string& body) {
    ChatError error;
    error.http_status = status;
    error.message = extract_error_message(
        body, "El servidor respondió con el estado HTTP " + std::to_string(status) + ".");

    error.kind = kind_for_http_status(status);
    return error;
}

/// Variante de map_http_error que ya conoce el valor crudo de Retry-After.
ChatError map_http_error(int status, const std::string& body,
                         const std::optional<std::string>& retry_after_raw) {
    ChatError error = map_http_error(status, body);
    // Retry-After aplica a todos los errores HTTP reintentables (429 y 5xx,
    // típicamente 503), con el mismo tope de kMaxDelay.
    if (is_retryable(error.kind)) {
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
        if (content.is_null()) {
            // Frecuente con modelos de razonamiento o llamadas a herramientas.
            return ChatError{ErrorKind::BadResponse, 0,
                             "El modelo no devolvió texto en la respuesta.", std::nullopt};
        }
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

/// Error de una petición cancelada (token, callback o transporte).
ChatError cancelled_error() {
    return ChatError{ErrorKind::Cancelled, 0, "Petición cancelada por el usuario.",
                     std::nullopt};
}

/// Indica si el token existe y ya se canceló.
bool is_cancelled(const CancelToken* cancel) {
    return cancel != nullptr && cancel->is_cancelled();
}

/// Fallo de transporte sin respuesta HTTP: red, timeout o cancelación.
ChatError transport_error(const HttpResponse& response) {
    ChatError error;
    if (response.cancelled) {
        return cancelled_error();
    }
    error.kind = response.timed_out ? ErrorKind::Timeout : ErrorKind::Network;
    error.message = !response.error.empty()
                        ? response.error
                        : std::string{error_kind_label(error.kind)};
    return error;
}

/// Indica si el texto crudo de un evento solo tiene comentarios (líneas que
/// empiezan con ':') o líneas vacías.
bool is_comment_only(const std::string& raw) {
    std::size_t begin = 0;
    while (begin <= raw.size()) {
        std::size_t end = raw.find('\n', begin);
        if (end == std::string::npos) {
            end = raw.size();
        }
        const std::string_view line{raw.data() + begin, end - begin};
        if (!line.empty() && line != "\r" && line.front() != ':') {
            return false;
        }
        if (end == raw.size()) {
            break;
        }
        begin = end + 1;
    }
    return true;
}

/// Fecha y hora local con formato AAAA-MM-DD HH:MM:SS.
std::string local_timestamp() {
    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm local{};
    if (localtime_r(&now, &local) == nullptr) {
        return "?";
    }
    std::ostringstream text;
    text << std::put_time(&local, "%Y-%m-%d %H:%M:%S");
    return text.str();
}

/// Volcado de depuración (CHAT_DEBUG_SSE): agrega un bloque por intento con
/// fecha, modelo, número de intento, estado HTTP y el cuerpo crudo de la
/// respuesta (label + detail, si label no está vacía, explica un corte). Nunca escribe
/// cabeceras, la key ni el cuerpo de la petición. Cualquier fallo se ignora:
/// depurar nunca debe romper una petición.
void append_debug_dump(const Config& config, int attempt, int status, std::string_view body,
                       std::string_view label, std::string_view detail) noexcept {
    if (!config.debug_sse_path.has_value()) {
        return;
    }
    try {
        std::ofstream file{*config.debug_sse_path, std::ios::app | std::ios::binary};
        if (!file.is_open()) {
            return;
        }
        file << "fecha: " << local_timestamp() << '\n'
             << "modelo: " << config.model << '\n'
             << "intento: " << attempt + 1 << '\n'
             << "estado HTTP: " << status << '\n';
        if (!label.empty()) {
            file << label << detail << '\n';
        }
        file << "cuerpo:\n"
             << body << '\n'
             << "========================================\n";
    } catch (...) {
        // Se ignora a propósito (ver arriba).
    }
}

/// Volcado de un intento a partir de la respuesta del transporte.
void append_debug_dump(const Config& config, int attempt, const HttpResponse& response) noexcept {
    if (response.status == 0) {
        // Sin respuesta HTTP no hay cuerpo: curl descarta lo parcial.
        append_debug_dump(config, attempt, 0, {}, "error de transporte: ", response.error);
        return;
    }
    append_debug_dump(config, attempt, response.status, response.body, {}, {});
}

/// Resultado de un intento de streaming ya consumido.
struct StreamAttemptOutcome {
    Result<CompletionInfo> result{CompletionInfo{}}; ///< Éxito o error final de la petición.
    bool delivered_any = false;   ///< Si ya se entregó al menos un delta.
    bool retryable = true;        ///< false si el fallo de red es permanente.
};

/// Ejecuta un intento de streaming: consume el cuerpo conforme llega con el
/// parser SSE y entrega los deltas por on_delta. Los errores HTTP no se
/// consumen como flujo: el cuerpo se acumula en la respuesta y se mapea al
/// final, igual que en complete().
StreamAttemptOutcome run_stream_attempt(const Config& config, int attempt,
                                        const HttpRequest& request, Transport& transport,
                                        const StreamDeltaCallback& on_delta) {
    StreamAttemptOutcome outcome;

    sse::Parser parser;
    std::string finish_reason; ///< Último choices[0].finish_reason no nulo.
    bool done = false;    ///< Se recibió data: [DONE].
    bool aborted = false; ///< Cuerpo inválido a mitad del flujo.
    std::optional<ChatError> abort_error;

    // Entrega el delta al callback de quien llama. Si lanza, la excepción
    // no debe llegar a libcurl (frames de C). Devuelve false para abortar.
    const auto deliver = [&](const std::string& text) {
        outcome.delivered_any = true;
        try {
            return on_delta(text); // false: cancelación, curl aborta con status == 0.
        } catch (const std::exception& e) {
            abort_error = ChatError{ErrorKind::Cancelled, 0,
                                    "El callback de streaming lanzó una excepción: " +
                                        std::string{e.what()},
                                    std::nullopt};
        } catch (...) {
            abort_error = ChatError{ErrorKind::Cancelled, 0,
                                    "El callback de streaming lanzó una excepción desconocida.",
                                    std::nullopt};
        }
        aborted = true;
        return false;
    };

    // Procesa los eventos completos que tenga el parser. Devuelve false si
    // hay que abortar la transferencia. final indica que es el fragmento que
    // quedó al terminar el flujo sin línea en blanco.
    const auto process_events = [&](bool final) {
        while (const std::optional<std::string> raw_event = parser.next_event()) {
            const Result<sse::Event> event = sse::decode_event(*raw_event);
            if (event.is_error()) {
                abort_error = event.error();
                aborted = true;
                return false;
            }
            if (event.value().type == sse::EventType::Done) {
                done = true;
                return true;
            }
            if (event.value().type != sse::EventType::Delta) {
                // Al final, un fragmento sin data que no es comentario es basura.
                if (final && !is_comment_only(*raw_event)) {
                    abort_error = ChatError{ErrorKind::BadResponse, 0,
                                            "El flujo terminó a mitad de un evento.",
                                            std::nullopt};
                    aborted = true;
                    return false;
                }
                continue;
            }
            const Result<std::optional<std::string>> delta =
                sse::decode_openai_chunk(event.value().data, &finish_reason);
            if (delta.is_error()) {
                abort_error = delta.error();
                aborted = true;
                return false;
            }
            if (delta.value().has_value() && !delta.value()->empty() &&
                !deliver(*delta.value())) {
                return false;
            }
        }
        return true;
    };

    // Para el volcado de depuración: lo que llegó, aunque la transferencia se
    // corte después (curl descarta el cuerpo parcial al abortar).
    const bool keep_raw = config.debug_sse_path.has_value();
    std::string raw_seen;
    int seen_status = 0;

    const HttpResponse response = transport.send_stream(
        request, [&](std::string_view raw, int chunk_status) {
            if (keep_raw) {
                raw_seen.append(raw);
                seen_status = chunk_status;
            }
            if (chunk_status < 200 || chunk_status > 299) {
                return true; // Error HTTP: se interpreta al final, no como flujo.
            }
            if (aborted || done) {
                return true;
            }
            parser.feed(raw);
            // Un trozo puede contener varios eventos: se procesan todos.
            return process_events(false);
        });
    if (response.status == 0 && seen_status != 0) {
        // La transferencia se cortó después de recibir datos (por ejemplo, un
        // evento de error dentro del flujo): se vuelca lo que llegó.
        append_debug_dump(config, attempt, seen_status, raw_seen,
                          "transferencia interrumpida: ", response.error);
    } else {
        append_debug_dump(config, attempt, response);
    }

    // Flujo 2xx completo que terminó sin línea en blanco: se cierra el último
    // evento y se procesa como cualquier otro ([DONE], chunk o basura).
    if (!aborted && !done && response.status >= 200 && response.status <= 299 &&
        parser.has_partial_data()) {
        parser.feed("\n\n");
        (void)process_events(true);
    }

    if (aborted) {
        outcome.result = Result<CompletionInfo>{*abort_error};
        return outcome;
    }
    if (response.status == 0) {
        // Red, timeout o cancelación desde el callback.
        outcome.result = transport_error(response);
        outcome.retryable = response.retryable;
        return outcome;
    }
    if (response.status < 200 || response.status > 299) {
        outcome.result = map_http_error(response.status, response.body,
                                        response.retry_after);
        return outcome;
    }
    // Con [DONE], o sin él pero con el flujo completo y limpio: éxito.
    outcome.result = CompletionInfo{finish_reason};
    return outcome;
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

Result<std::string> ChatClient::complete(const std::vector<Message>& messages,
                                         const CancelToken* cancel) {
    // Revalida la configuración: el cliente puede construirse con un Config
    // armado a mano (decisión del usuario: validar en ambos lados).
    if (const std::optional<ChatError> config_error = validate_config(config_)) {
        return *config_error;
    }

    const Result<std::string> body = build_request_body(config_.model, messages, false);
    if (body.is_error()) {
        return Result<std::string>{body.error()};
    }

    HttpRequest request;
    request.url = build_url(config_.base_url);
    request.body = body.value();
    request.api_key = config_.api_key;
    request.timeout = std::chrono::milliseconds(config_.timeout_seconds);
    request.cancel = cancel;

    for (int attempt = 0;; ++attempt) {
        if (is_cancelled(cancel)) {
            return cancelled_error();
        }
        const HttpResponse response = transport_->send(request);
        append_debug_dump(config_, attempt, response);

        // Sin respuesta HTTP: fallo de red, timeout de curl o cancelación.
        if (response.status == 0) {
            ChatError error = transport_error(response);
            if (attempt < kMaxRetries && is_retryable(error.kind) && response.retryable) {
                if (!sleeper().sleep_for(kBaseDelay * (1 << attempt), cancel)) {
                    return cancelled_error();
                }
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
            if (!sleeper().sleep_for(wait, cancel)) {
                return cancelled_error();
            }
            continue;
        }
        return error;
    }
}

Result<CompletionInfo> ChatClient::complete_stream(const std::vector<Message>& messages,
                                         const StreamDeltaCallback& on_delta,
                                         const CancelToken* cancel) {
    if (const std::optional<ChatError> config_error = validate_config(config_)) {
        return *config_error;
    }

    const Result<std::string> body = build_request_body(config_.model, messages, true);
    if (body.is_error()) {
        return Result<CompletionInfo>{body.error()};
    }

    HttpRequest request;
    request.url = build_url(config_.base_url);
    request.body = body.value();
    request.api_key = config_.api_key;
    request.timeout = std::chrono::milliseconds(config_.timeout_seconds);
    request.cancel = cancel;

    for (int attempt = 0;; ++attempt) {
        if (is_cancelled(cancel)) {
            return cancelled_error();
        }
        StreamAttemptOutcome outcome = run_stream_attempt(config_, attempt, request, *transport_, on_delta);

        const bool can_retry = attempt < kMaxRetries &&
                               outcome.result.is_error() &&
                               is_retryable(outcome.result.error().kind) &&
                               outcome.retryable && !outcome.delivered_any;

        if (!can_retry) {
            return outcome.result;
        }

        const ChatError& error = outcome.result.error();
        std::chrono::milliseconds wait{kBaseDelay * (1 << attempt)};
        if (error.retry_after.has_value()) {
            wait = *error.retry_after;
        }
        if (!sleeper().sleep_for(wait, cancel)) {
            return cancelled_error();
        }
    }
}

} // namespace chatbot
