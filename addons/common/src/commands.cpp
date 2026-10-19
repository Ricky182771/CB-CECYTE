#include "chatbot/bot/commands.h"

#include <algorithm>
#include <cctype>

namespace chatbot::bot {
namespace {

bool is_space(char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; }

std::string_view trim(std::string_view text) {
    while (!text.empty() && is_space(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && is_space(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

} // namespace

BotCommand parse_bot_command(std::string_view text) {
    const std::string_view input = trim(text);
    BotCommand normal{BotCommand::Type::Normal, std::string{input}};
    if (input.empty() || input.front() != '/') {
        return normal;
    }
    const auto name_end = static_cast<std::size_t>(
        std::find_if(input.begin(), input.end(), is_space) - input.begin());
    std::string_view name = input.substr(0, name_end);
    // "/start@MiBot" en los grupos de Telegram.
    if (const std::size_t at = name.find('@'); at != std::string_view::npos) {
        name = name.substr(0, at);
    }
    const std::string_view rest = trim(input.substr(name_end));

    if (name == kStartCommand) {
        // El código es la primera palabra.
        const auto word_end = static_cast<std::size_t>(
            std::find_if(rest.begin(), rest.end(), is_space) - rest.begin());
        return {BotCommand::Type::Start, std::string{rest.substr(0, word_end)}};
    }
    if (name == kHelpCommand) {
        return {BotCommand::Type::Help, {}};
    }
    if (name == kNewCommand) {
        return {BotCommand::Type::New, {}};
    }
    if (name == kSearchCommand) {
        if (rest.empty()) {
            return {BotCommand::Type::SearchEmpty, {}};
        }
        // Tavily recibe una sola línea: los saltos pasan a espacios.
        std::string query{rest};
        std::replace(query.begin(), query.end(), '\n', ' ');
        std::replace(query.begin(), query.end(), '\r', ' ');
        return {BotCommand::Type::Search, std::move(query)};
    }
    return normal;
}

} // namespace chatbot::bot
