#include "chatbot/addon_settings/invited.h"

#include "chatbot/error.h"

#include <nlohmann/json.hpp>

#include <exception>
#include <optional>

namespace chatbot::addon_settings::detail {

Result<std::vector<std::int64_t>> parse_invited(std::string_view path, std::string_view text) {
    const auto damaged = [&path](const std::string& why) {
        return ChatError{ErrorKind::BadResponse, 0,
                         std::string{path} + " está dañado (" + why +
                             "). No se tocó: arréglalo o quítalo de ahí (los invitados tendrán "
                             "que volver a usar el código).",
                         std::nullopt};
    };
    try {
        nlohmann::json document;
        try {
            document = nlohmann::json::parse(text);
        } catch (const nlohmann::json::exception&) {
            return damaged("no es JSON válido");
        }
        if (!document.is_object() || document.value("version", 0) != 1) {
            return damaged("no es la versión 1");
        }
        const auto users = document.find("users");
        if (users == document.end() || !users->is_array()) {
            return damaged("falta la lista \"users\"");
        }
        std::vector<std::int64_t> out;
        for (const nlohmann::json& user : *users) {
            if (!user.is_number_integer() || user.get<std::int64_t>() <= 0) {
                return damaged("un ID no es un número mayor que 0");
            }
            out.push_back(user.get<std::int64_t>());
        }
        return out;
    } catch (const std::exception&) {
        return damaged("no se pudo interpretar");
    }
}

std::string format_invited(const std::vector<std::int64_t>& users) {
    const nlohmann::json document{{"version", 1}, {"users", users}};
    return document.dump(2) + "\n";
}

} // namespace chatbot::addon_settings::detail
