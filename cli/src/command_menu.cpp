#include "command_menu.h"

#include <algorithm>
#include <cctype>
#include <utility>

namespace chatbot::cli {

namespace {

bool is_space(char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; }

char lower(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }

/// Se está escribiendo el nombre de un comando: "/" y nada de espacios.
bool typing_command_name(std::string_view text) {
    return !text.empty() && text.front() == '/' &&
           std::none_of(text.begin(), text.end(), is_space);
}

/// name empieza con typed, sin distinguir mayúsculas (los nombres son ASCII).
bool matches_prefix(std::string_view name, std::string_view typed) {
    return typed.size() <= name.size() &&
           std::equal(typed.begin(), typed.end(), name.begin(),
                      [](char a, char b) { return lower(a) == lower(b); });
}

} // namespace

void CommandMenu::update(std::string_view text, bool input_focused) {
    if (dismissed_.has_value() && *dismissed_ != text) {
        dismissed_.reset();
    }
    text_ = std::string{text};
    open_ = input_focused && !dismissed_.has_value() && typing_command_name(text);
    if (!open_) {
        // Al volver a abrirse, el cursor empieza arriba.
        matches_.clear();
        cursor_ = 0;
        return;
    }
    // El cursor sigue en el mismo comando si todavía coincide; si no, arriba.
    const std::optional<std::size_t> previous =
        cursor_ < matches_.size() ? std::optional<std::size_t>{matches_[cursor_]} : std::nullopt;
    matches_.clear();
    for (std::size_t i = 0; i < kCommands.size(); ++i) {
        if (matches_prefix(kCommands[i].name, text)) {
            matches_.push_back(i);
        }
    }
    const auto kept = std::find(matches_.begin(), matches_.end(), previous.value_or(kCommands.size()));
    cursor_ = kept != matches_.end() ? static_cast<std::size_t>(kept - matches_.begin()) : 0;
}

void CommandMenu::select(std::size_t row) {
    if (open_ && row < matches_.size()) {
        cursor_ = row;
    }
}

CommandMenuAction CommandMenu::choose(std::size_t row) {
    if (!open_ || row >= matches_.size()) {
        return {};
    }
    return complete(row, false);
}

CommandMenuAction CommandMenu::handle(ListKey key) {
    if (!open_) {
        return {};
    }
    if (key == ListKey::Escape) {
        dismiss(text_);
        return {CommandMenuAction::Type::Handled, {}};
    }
    if (matches_.empty()) {
        return {};
    }
    const std::size_t last = matches_.size() - 1;
    switch (key) {
    case ListKey::Up:
        cursor_ = cursor_ > 0 ? cursor_ - 1 : last;
        break;
    case ListKey::Down:
        cursor_ = cursor_ < last ? cursor_ + 1 : 0;
        break;
    case ListKey::Home:
    case ListKey::PageUp:
        cursor_ = 0;
        break;
    case ListKey::End:
    case ListKey::PageDown:
        cursor_ = last;
        break;
    case ListKey::Tab:
        return complete(cursor_, false);
    case ListKey::Enter:
        return complete(cursor_, true);
    case ListKey::Escape:
    case ListKey::Delete:
    case ListKey::Other:
        return {};
    }
    return {CommandMenuAction::Type::Handled, {}};
}

CommandMenuAction CommandMenu::complete(std::size_t row, bool submit_if_possible) {
    const CommandInfo& command = kCommands[matches_[row]];
    if (submit_if_possible && !command.needs_argument) {
        std::string text{command.name};
        dismiss(text);
        return {CommandMenuAction::Type::Submit, std::move(text)};
    }
    std::string text = std::string{command.name} + (command.arguments.empty() ? "" : " ");
    dismiss(text);
    return {CommandMenuAction::Type::Complete, std::move(text)};
}

void CommandMenu::dismiss(std::string text) {
    dismissed_ = std::move(text);
    open_ = false;
}

} // namespace chatbot::cli
