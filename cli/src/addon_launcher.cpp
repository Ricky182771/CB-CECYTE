#include "addon_launcher.h"

#include "child_process.h"

#include "chatbot/config.h"

#include <iostream>

namespace chatbot::cli {

namespace settings = addon_settings;

CommandLine parse_command_line(const std::vector<std::string>& args) {
    CommandLine line;
    if (args.empty()) {
        return line;
    }
    const std::string& first = args.front();
    if (first == "--help" || first == "-h") {
        if (args.size() == 1) {
            line.kind = CommandLine::Kind::Help;
        } else {
            line.kind = CommandLine::Kind::Usage;
            line.problem = first + " no lleva más argumentos.";
        }
        return line;
    }
    if (const settings::AddonInfo* addon = settings::find_addon(first)) {
        line.kind = CommandLine::Kind::Addon;
        line.addon = addon;
        line.arguments.assign(args.begin() + 1, args.end());
        return line;
    }
    line.kind = CommandLine::Kind::Usage;
    line.problem = "argumento desconocido: " + first;
    return line;
}

std::optional<std::string> find_addon_executable(const settings::AddonInfo& addon,
                                                 const std::string& executable_dir,
                                                 bool portable,
                                                 const ExecutableCheck& is_executable,
                                                 const PathSearch& search_path) {
    if (!executable_dir.empty()) {
        const std::string beside = executable_dir + "/" + std::string{addon.executable};
        if (is_executable(beside)) {
            return beside;
        }
    }
    if (portable) {
        return std::nullopt;
    }
    return search_path(addon.executable);
}

std::optional<std::string> locate_addon(const settings::AddonInfo& addon) {
    if (current_os() == Os::Windows) {
        return std::nullopt;
    }
    return find_addon_executable(addon, executable_dir().value_or(""),
                                 process_portable_data_dir().has_value(), is_executable_file,
                                 find_in_path);
}

std::string addon_not_found_message(const settings::AddonInfo& addon, bool portable) {
    return "No encontré " + std::string{addon.executable} + " junto a chatbot" +
           (portable ? "." : " ni en el PATH.");
}

std::string usage_text(Os os, bool portable, const AddonLocator& locate) {
    std::string text =
        "Uso:\n"
        "  chatbot                       abre el chat\n"
        "  chatbot <addon> [argumentos]  arranca un addon\n"
        "  chatbot --help                esta ayuda\n"
        "\n"
        "Addons:\n";
    if (os == Os::Windows) {
        return text + "  " + std::string{settings::kAddonsUnavailableOnWindows} + "\n";
    }
    for (const settings::AddonInfo& addon : settings::known_addons()) {
        text += "  " + std::string{addon.name} + "  " + std::string{addon.title} + ": ";
        if (const std::optional<std::string> path = locate(addon)) {
            text += "instalado en " + *path + "\n";
        } else {
            text += "no instalado (copia " + std::string{addon.executable} + " junto a chatbot" +
                    (portable ? "" : " o en el PATH") + ")\n";
        }
    }
    return text;
}

std::optional<int> run_command_line(const std::vector<std::string>& args) {
    const CommandLine line = parse_command_line(args);
    const bool portable = process_portable_data_dir().has_value();
    switch (line.kind) {
    case CommandLine::Kind::Interface:
        return std::nullopt;
    case CommandLine::Kind::Help:
        std::cout << usage_text(current_os(), portable, locate_addon);
        return 0;
    case CommandLine::Kind::Usage:
        std::cerr << "chatbot: " << line.problem << "\n\n"
                  << usage_text(current_os(), portable, locate_addon);
        return kExitUsage;
    case CommandLine::Kind::Addon:
        break;
    }
    if (current_os() == Os::Windows) {
        std::cerr << "chatbot: " << settings::kAddonsUnavailableOnWindows << '\n';
        return 1;
    }
    const std::optional<std::string> path = locate_addon(*line.addon);
    if (!path.has_value()) {
        std::cerr << "chatbot: " << addon_not_found_message(*line.addon, portable) << '\n';
        return kExitUnavailable;
    }
    std::vector<std::string> argv{*path};
    argv.insert(argv.end(), line.arguments.begin(), line.arguments.end());
    const std::string error = exec_program(*path, argv);
    std::cerr << "chatbot: no se pudo arrancar " << *path << ": " << error << '\n';
    return 1;
}

} // namespace chatbot::cli
