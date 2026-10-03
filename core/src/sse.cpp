#include "sse.h"

#include "error_body.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace chatbot {
namespace sse {
namespace {

using nlohmann::json;

/// Separa "data:valor" (con o sin espacio tras los dos puntos, como permite
/// la especificación). Nullopt si la línea no es del campo data.
std::optional<std::string_view> data_field_of(std::string_view line) {
    if (line.substr(0, 5) != "data:") {
        return std::nullopt;
    }
    std::string_view value = line.substr(5);
    if (!value.empty() && value.front() == ' ') {
        value.remove_prefix(1);
    }
    return value;
}

/// Une los valores de los campos data de un evento con '\n'.
std::string join_data_lines(const std::string& raw) {
    std::string data;
    std::size_t begin = 0;
    while (begin <= raw.size()) {
        std::size_t end = raw.find('\n', begin);
        if (end == std::string::npos) {
            end = raw.size();
        }
        // Un '\r' final se descarta (líneas CRLF, sección 8 de la spec SSE).
        std::size_t line_end = end;
        if (line_end > begin && raw[line_end - 1] == '\r') {
            --line_end;
        }
        const std::string_view line{raw.data() + begin, line_end - begin};
        if (const std::optional<std::string_view> value = data_field_of(line)) {
            if (!data.empty()) {
                data.push_back('\n');
            }
            data.append(*value);
        }
        if (end == raw.size()) {
            break;
        }
        begin = end + 1;
    }
    return data;
}

/// error.code como entero: número JSON o cadena de solo dígitos.
std::optional<int> numeric_code(const json& error) {
    const auto code_it = error.find("code");
    if (code_it == error.end()) {
        return std::nullopt;
    }
    if (code_it->is_number_integer()) {
        const long long value = code_it->get<long long>();
        if (value >= 0 && value <= 999) {
            return static_cast<int>(value);
        }
        return std::nullopt;
    }
    if (code_it->is_string()) {
        const std::string& text = code_it->get_ref<const std::string&>();
        if (text.empty() || text.size() > 3) {
            return std::nullopt;
        }
        int value = 0;
        for (const char c : text) {
            if (c < '0' || c > '9') {
                return std::nullopt;
            }
            value = value * 10 + (c - '0');
        }
        return value;
    }
    return std::nullopt;
}

/// Indica si texto (ya en minúsculas) contiene alguna de las palabras.
bool contains_any(const std::string& text, std::initializer_list<std::string_view> words) {
    return std::any_of(words.begin(), words.end(), [&text](std::string_view word) {
        return text.find(word) != std::string::npos;
    });
}

/// Clasifica un error que llegó dentro del flujo SSE (con estado HTTP 200),
/// para poder reintentar la sobrecarga del servidor:
/// - error.code numérico: igual que un estado HTTP;
/// - si no, error.type sin distinguir mayúsculas: overload/unavailable/server
///   → Server; rate/exhausted → RateLimited;
/// - si no aplica nada: BadResponse.
ErrorKind classify_stream_error(const json& error) {
    if (!error.is_object()) {
        return ErrorKind::BadResponse;
    }
    if (const std::optional<int> code = numeric_code(error)) {
        return kind_for_http_status(*code);
    }
    const auto type_it = error.find("type");
    if (type_it != error.end() && type_it->is_string()) {
        std::string type = type_it->get<std::string>();
        std::transform(type.begin(), type.end(), type.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        if (contains_any(type, {"overload", "unavailable", "server"})) {
            return ErrorKind::Server;
        }
        if (contains_any(type, {"rate", "exhausted"})) {
            return ErrorKind::RateLimited;
        }
    }
    return ErrorKind::BadResponse;
}

} // namespace

void Parser::feed(std::string_view raw) {
    buffer_.append(raw);

    std::size_t search_from = 0;
    for (;;) {
        // El separador de eventos es una línea en blanco: "\n\n" o "\r\n\r\n".
        const std::size_t lf = buffer_.find("\n\n", search_from);
        const std::size_t crlf = buffer_.find("\r\n\r\n", search_from);
        const bool use_crlf = crlf != std::string::npos && (lf == std::string::npos || crlf < lf);
        if (lf == std::string::npos && crlf == std::string::npos) {
            break;
        }
        const std::size_t separator = use_crlf ? crlf : lf;
        const std::size_t skip = use_crlf ? 4 : 2;
        pending_.push_back(buffer_.substr(0, separator));
        buffer_.erase(0, separator + skip);
        search_from = 0;
    }
}

std::optional<std::string> Parser::next_event() {
    if (pending_.empty()) {
        return std::nullopt;
    }
    std::string event = std::move(pending_.front());
    pending_.pop_front();
    return event;
}

bool Parser::has_partial_data() const {
    return !buffer_.empty();
}

Result<Event> decode_event(const std::string& raw) {
    const std::string data = join_data_lines(raw);
    if (data.empty()) {
        return Event{EventType::Comment, {}};
    }
    if (data == "[DONE]") {
        return Event{EventType::Done, {}};
    }
    return Event{EventType::Delta, std::move(data)};
}

Result<std::optional<std::string>> decode_openai_chunk(const std::string& data,
                                                       std::string* finish_reason) {
    json document;
    try {
        document = json::parse(data);
    } catch (const json::exception& e) {
        return ChatError{ErrorKind::BadResponse, 0,
                         "JSON malformado en el flujo SSE: " + std::string{e.what()},
                         std::nullopt};
    }
    if (!document.is_object()) {
        return ChatError{ErrorKind::BadResponse, 0,
                         "El chunk del flujo no es un objeto JSON.", std::nullopt};
    }

    // El error se revisa antes que choices: el servidor puede reportar un
    // fallo dentro del flujo aunque el estado HTTP haya sido 200.
    if (const auto error_it = document.find("error");
        error_it != document.end() && !error_it->is_null()) {
        return ChatError{classify_stream_error(*error_it), 0,
                         extract_error_message(
                             data, "El servidor reportó un error dentro del flujo."),
                         std::nullopt};
    }

    const auto choices_it = document.find("choices");
    if (choices_it == document.end() || !choices_it->is_array()) {
        return ChatError{ErrorKind::BadResponse, 0,
                         "El chunk del flujo no contiene \"choices\".", std::nullopt};
    }
    if (choices_it->empty()) {
        // Chunk solo con estadísticas (usage) al final del flujo: sin texto.
        return std::optional<std::string>{std::nullopt};
    }
    const json& first = choices_it->front();
    if (!first.is_object() || !first.contains("delta")) {
        return ChatError{ErrorKind::BadResponse, 0,
                         "El chunk del flujo no contiene \"delta\".", std::nullopt};
    }
    if (finish_reason != nullptr) {
        const auto reason_it = first.find("finish_reason");
        if (reason_it != first.end() && reason_it->is_string()) {
            *finish_reason = reason_it->get<std::string>();
        }
    }
    const json& delta = first.at("delta");
    if (!delta.is_object()) {
        return ChatError{ErrorKind::BadResponse, 0,
                         "\"choices[0].delta\" no es un objeto.", std::nullopt};
    }
    if (!delta.contains("content")) {
        // Chunk sin texto (p. ej. el cierre con finish_reason): no aporta nada.
        return std::optional<std::string>{std::nullopt};
    }
    const json& content = delta.at("content");
    if (content.is_null()) {
        // Primer chunk con solo role, reasoning_content o tool_calls: sin texto.
        return std::optional<std::string>{std::nullopt};
    }
    if (!content.is_string()) {
        return ChatError{ErrorKind::BadResponse, 0,
                         "\"choices[0].delta.content\" no es una cadena.", std::nullopt};
    }
    return std::optional<std::string>{content.get<std::string>()};
}

} // namespace sse
} // namespace chatbot
