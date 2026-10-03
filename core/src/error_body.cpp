#include "error_body.h"

#include <nlohmann/json.hpp>

#include <cstddef>

namespace chatbot {
namespace {

using nlohmann::json;

/// Longitud máxima del mensaje extraído del cuerpo de un error.
constexpr std::size_t kMaxErrorMessageLength = 300;

} // namespace

ErrorKind kind_for_http_status(int status) {
    switch (status) {
    case 401:
    case 403:
        return ErrorKind::Auth;
    case 404:
    case 410:
        return ErrorKind::ModelNotFound;
    case 429:
        return ErrorKind::RateLimited;
    case 400:
    case 413:
    case 422:
        return ErrorKind::InvalidRequest;
    default:
        return status >= 500 && status <= 599 ? ErrorKind::Server : ErrorKind::BadResponse;
    }
}

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
