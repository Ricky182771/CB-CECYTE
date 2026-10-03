#include "sse.h"

#include <nlohmann/json.hpp>

#include <cstddef>
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

Result<std::optional<std::string>> decode_openai_chunk(const std::string& data) {
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

    const auto choices_it = document.find("choices");
    if (choices_it == document.end() || !choices_it->is_array() || choices_it->empty()) {
        return ChatError{ErrorKind::BadResponse, 0,
                         "El chunk del flujo no contiene \"choices\".", std::nullopt};
    }
    const json& first = choices_it->front();
    if (!first.is_object() || !first.contains("delta")) {
        return ChatError{ErrorKind::BadResponse, 0,
                         "El chunk del flujo no contiene \"delta\".", std::nullopt};
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
    if (!content.is_string()) {
        return ChatError{ErrorKind::BadResponse, 0,
                         "\"choices[0].delta.content\" no es una cadena.", std::nullopt};
    }
    return std::optional<std::string>{content.get<std::string>()};
}

} // namespace sse
} // namespace chatbot
