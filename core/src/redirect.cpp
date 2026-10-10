#include "redirect.h"

#include "chatbot/config.h"
#include "chatbot/utf8.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace chatbot {
namespace {

constexpr std::string_view kEllipsis = "…";

/// La URL hasta la primera "?" o "#".
std::string_view without_query(std::string_view url) {
    return url.substr(0, url.find_first_of("?#"));
}

/// Sin "/" al final (build_url también las quita).
std::string_view without_trailing_slashes(std::string_view url) {
    while (!url.empty() && url.back() == '/') {
        url.remove_suffix(1);
    }
    return url;
}

bool is_control(std::uint32_t code) {
    return code < 0x20 || code == 0x7F || (code >= 0x80 && code <= 0x9F);
}

/// Sin controles y con U+FFFD en lugar de los bytes inválidos.
std::string clean_text(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size()) {
        const std::size_t begin = i;
        const std::optional<std::uint32_t> code = utf8::next_code_point(text, i);
        if (!code.has_value()) {
            out += utf8::kReplacement;
            ++i;
            continue;
        }
        if (!is_control(*code)) {
            out.append(text.substr(begin, i - begin));
        }
    }
    return out;
}

/// Quita "usuario[:contraseña]@" de la autoridad.
std::string without_user_info(std::string url) {
    const std::size_t separator = url.find("://");
    if (separator == std::string::npos) {
        return url;
    }
    const std::size_t begin = separator + 3;
    const std::size_t end = std::min(url.find('/', begin), url.size());
    const std::size_t at = std::string_view{url}.substr(begin, end - begin).rfind('@');
    if (at != std::string_view::npos) {
        url.erase(begin, at + 1);
    }
    return url;
}

} // namespace

std::string redirect_display(std::string_view redirect_url) {
    std::string text = without_user_info(clean_text(without_query(redirect_url)));
    if (text.size() <= kMaxRedirectDisplayBytes) {
        return text;
    }
    // Ya es UTF-8 válido: se retrocede hasta el inicio de un carácter.
    std::size_t cut = kMaxRedirectDisplayBytes - kEllipsis.size();
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) {
        --cut;
    }
    text.resize(cut);
    text += kEllipsis;
    return text;
}

std::optional<std::string> suggest_base_url(std::string_view request_url,
                                            std::string_view redirect_url,
                                            std::string_view path) {
    const std::string_view target = without_query(redirect_url);
    if (path.empty() || target.size() <= path.size() ||
        target.substr(target.size() - path.size()) != path) {
        return std::nullopt;
    }
    const std::string suggestion{
        without_trailing_slashes(target.substr(0, target.size() - path.size()))};
    // Lo que no se mostraría tal cual (controles, bytes inválidos, usuario,
    // demasiado larga) no se sugiere.
    if (validate_base_url(suggestion).has_value() || redirect_display(suggestion) != suggestion) {
        return std::nullopt;
    }
    std::string_view current = without_query(request_url);
    if (current.size() >= path.size() && current.substr(current.size() - path.size()) == path) {
        current.remove_suffix(path.size());
    }
    if (without_trailing_slashes(current) == suggestion) {
        return std::nullopt;
    }
    return suggestion;
}

} // namespace chatbot
