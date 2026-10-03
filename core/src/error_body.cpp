#include "error_body.h"

#include <nlohmann/json.hpp>

#include <cstddef>

namespace chatbot {
namespace {

using nlohmann::json;

/// Longitud máxima del mensaje extraído del cuerpo de un error.
constexpr std::size_t kMaxErrorMessageLength = 300;

} // namespace

std::string truncate_message(const std::string& text) {
    if (text.size() <= kMaxErrorMessageLength) {
        return text;
    }
    return text.substr(0, kMaxErrorMessageLength) + "...";
}

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

} // namespace chatbot
