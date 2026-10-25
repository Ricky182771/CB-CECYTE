#include "chatbot/addon_settings/paths.h"

#include "chatbot/config.h"

#include <cstdlib>

namespace chatbot::addon_settings {
namespace {

/// Una variable de entorno, o nullopt si no está.
std::optional<std::string> env(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr ? std::optional<std::string>{value} : std::nullopt;
}

/// La variable XDG solo si es ruta absoluta, sin las barras finales.
std::optional<std::string> absolute_base(const std::optional<std::string>& value) {
    if (!value.has_value() || value->empty() || value->front() != '/') {
        return std::nullopt;
    }
    std::string base = *value;
    while (base.size() > 1 && base.back() == '/') {
        base.pop_back();
    }
    return base;
}

} // namespace

std::optional<std::string> resolve_addon_data_dir(std::string_view name,
                                                  const std::optional<std::string>& portable_dir,
                                                  const std::optional<std::string>& xdg_data_home,
                                                  const std::optional<std::string>& home) {
    if (portable_dir.has_value() && !portable_dir->empty()) {
        return *portable_dir + "/" + std::string{name};
    }
    if (const std::optional<std::string> base = absolute_base(xdg_data_home)) {
        return *base + "/chatbot/" + std::string{name};
    }
    if (home.has_value() && !home->empty()) {
        return *home + "/.local/share/chatbot/" + std::string{name};
    }
    return std::nullopt;
}

std::optional<std::string> default_addon_data_dir(std::string_view name) {
    return resolve_addon_data_dir(name, process_portable_data_dir(), env("XDG_DATA_HOME"),
                                  env("HOME"));
}

std::string invited_file_path(std::string_view data_dir) {
    return std::string{data_dir} + "/invitados.json";
}

std::optional<std::string> resolve_unit_path(std::string_view unit,
                                             const std::optional<std::string>& home) {
    if (home.has_value() && !home->empty()) {
        return *home + "/.config/systemd/user/" + std::string{unit};
    }
    return std::nullopt;
}

std::optional<std::string> default_unit_path(std::string_view unit) {
    return resolve_unit_path(unit, env("HOME"));
}

} // namespace chatbot::addon_settings
