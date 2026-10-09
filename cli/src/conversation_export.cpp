#include "conversation_export.h"

#include "chatbot/web_search.h"

#include <algorithm>
#include <cstddef>
#include <vector>

namespace chatbot::cli {

namespace {

/// En una sola línea: los saltos de línea pasan a espacios.
std::string one_line(std::string_view text) {
    std::string out{text};
    std::replace(out.begin(), out.end(), '\n', ' ');
    std::replace(out.begin(), out.end(), '\r', ' ');
    return out;
}

/// Sin saltos de línea al final (entre secciones va siempre una línea vacía).
std::string_view without_final_newlines(std::string_view text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.remove_suffix(1);
    }
    return text;
}

/// Texto de un enlace: "\", "[" y "]" escapados para no cerrar el enlace.
std::string link_text(std::string_view text) {
    std::string out;
    for (const char c : one_line(text)) {
        if (c == '\\' || c == '[' || c == ']') {
            out += '\\';
        }
        out += c;
    }
    return out;
}

/// Destino de un enlace: espacios, paréntesis y "<" ">" codificados como
/// %XX para que el enlace no se corte.
std::string link_target(std::string_view url) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    for (const char c : url) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte <= 0x20U || byte == 0x7FU || c == '(' || c == ')' || c == '<' || c == '>') {
            out += '%';
            out += kHex[byte >> 4U];
            out += kHex[byte & 0x0FU];
        } else {
            out += c;
        }
    }
    return out;
}

std::string source_line(std::size_t number, const SearchResult& source) {
    const std::string title = one_line(source.title);
    const std::string url = one_line(source.url);
    const std::string date = one_line(source.published_date);
    std::string line = std::to_string(number) + ". ";
    if (is_web_url(source.url)) {
        line += "[" + link_text(title.empty() ? url : title) + "](" + link_target(source.url) + ")";
        if (!date.empty()) {
            line += " — " + date;
        }
        return line;
    }
    // Segunda defensa (TavilySearch y el lector ya las descartan): sin enlace.
    line += title.empty() ? url : title;
    if (!date.empty()) {
        line += " — " + date;
    }
    if (!title.empty()) {
        line += " (" + url + ")";
    }
    return line;
}

} // namespace

std::string export_date(std::string_view iso8601) {
    // "AAAA-MM-DDTHH:MM..." → "AAAA-MM-DD HH:MM".
    if (iso8601.size() >= 16 && iso8601[4] == '-' && iso8601[7] == '-' && iso8601[10] == 'T' &&
        iso8601[13] == ':') {
        return std::string{iso8601.substr(0, 10)} + " " + std::string{iso8601.substr(11, 5)};
    }
    return std::string{iso8601};
}

std::string export_markdown(const StoredConversation& conversation,
                            std::string_view fallback_date) {
    const std::string title = one_line(conversation.title);
    std::string out = "# " + (title.empty() ? std::string{"Conversación"} : title) + "\n\n";

    std::vector<std::string> models;
    for (const StoredMessage& message : conversation.messages) {
        if (message.role == Role::Assistant && !message.model.empty() &&
            std::find(models.begin(), models.end(), message.model) == models.end()) {
            models.push_back(message.model);
        }
    }
    out += conversation.created_at.empty() ? std::string{fallback_date}
                                           : export_date(conversation.created_at);
    if (!models.empty()) {
        out += models.size() == 1 ? " · Modelo: " : " · Modelos: ";
        for (std::size_t i = 0; i < models.size(); ++i) {
            out += (i > 0 ? ", " : "") + one_line(models[i]);
        }
    }
    out += "\n";

    // La búsqueda va con el mensaje del usuario; sus fuentes, después de la respuesta.
    const StoredSearch* pending_search = nullptr;
    for (const StoredMessage& message : conversation.messages) {
        if (message.role == Role::User) {
            out += "\n## Tú\n\n" + std::string{without_final_newlines(message.content)} + "\n";
            pending_search = message.search.has_value() ? &*message.search : nullptr;
        } else if (message.role == Role::Assistant) {
            out += "\n## Asistente\n\n" + std::string{without_final_newlines(message.content)} +
                   "\n";
            if (pending_search != nullptr && !pending_search->response.results.empty()) {
                out += "\n### Fuentes\n\n";
                const std::vector<SearchResult>& results = pending_search->response.results;
                for (std::size_t i = 0; i < results.size(); ++i) {
                    out += source_line(i + 1, results[i]) + "\n";
                }
            }
            pending_search = nullptr;
        }
        // Role::System nunca se guarda; si apareciera, no se exporta.
    }
    return out;
}

std::string export_file_name(std::string_view id, std::time_t now) {
    if (!id.empty()) {
        return "conversacion-" + std::string{id} + ".md";
    }
    std::tm local{};
    localtime_r(&now, &local);
    char stamp[32] = {};
    const std::size_t size = std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &local);
    return "conversacion-" + std::string{stamp, size} + ".md";
}

} // namespace chatbot::cli
