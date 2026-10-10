#include "url.h"

#include <algorithm>

namespace chatbot {

namespace {

std::string lower(std::string_view text) {
    std::string out{text};
    std::transform(out.begin(), out.end(), out.begin(), [](char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    });
    return out;
}

} // namespace

std::optional<ParsedUrl> parse_url(std::string_view url) {
    const std::size_t separator = url.find("://");
    if (separator == std::string_view::npos || separator == 0) {
        return std::nullopt;
    }
    ParsedUrl parsed;
    parsed.scheme = lower(url.substr(0, separator));
    for (const char c : parsed.scheme) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '-' ||
              c == '.')) {
            return std::nullopt;
        }
    }
    // La autoridad termina en la primera "/", "?" o "#".
    std::string_view rest = url.substr(separator + 3);
    const std::string_view authority = rest.substr(0, rest.find_first_of("/?#"));
    if (authority.empty() || authority.find('@') != std::string_view::npos) {
        return std::nullopt; // Sin host, o con usuario: "http://localhost@evil.com".
    }
    std::string_view host;
    std::string_view port_text;
    if (authority.front() == '[') {
        const std::size_t close = authority.find(']');
        if (close == std::string_view::npos) {
            return std::nullopt;
        }
        host = authority.substr(0, close + 1);
        const std::string_view after = authority.substr(close + 1);
        if (!after.empty()) {
            if (after.front() != ':') {
                return std::nullopt;
            }
            port_text = after.substr(1);
            if (port_text.empty()) {
                return std::nullopt;
            }
        }
    } else {
        const std::size_t colon = authority.find(':');
        host = authority.substr(0, colon);
        if (colon != std::string_view::npos) {
            port_text = authority.substr(colon + 1);
            if (port_text.empty()) {
                return std::nullopt;
            }
        }
    }
    if (host.empty() || host == "[]") {
        return std::nullopt;
    }
    if (!port_text.empty()) {
        int port = 0;
        for (const char c : port_text) {
            if (c < '0' || c > '9' || port > 65535) {
                return std::nullopt;
            }
            port = port * 10 + (c - '0');
        }
        if (port < 1 || port > 65535) {
            return std::nullopt;
        }
        parsed.port = port;
    }
    parsed.host = lower(host);
    // La ruta va de la autoridad a la primera "?" o "#".
    const std::string_view after_authority = rest.substr(authority.size());
    parsed.path = std::string{after_authority.substr(0, after_authority.find_first_of("?#"))};
    return parsed;
}

bool is_local_host(std::string_view host) {
    const std::string normalized = lower(host);
    return normalized == "localhost" || normalized == "127.0.0.1" || normalized == "[::1]";
}

} // namespace chatbot
