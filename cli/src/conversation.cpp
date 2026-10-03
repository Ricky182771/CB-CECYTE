#include "conversation.h"

#include <cctype>
#include <cstddef>
#include <utility>

namespace chatbot::cli {
namespace {

/// Quita espacios, tabuladores y saltos de línea de los extremos.
std::string_view trim(std::string_view text) {
    const auto is_space = [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; };
    std::size_t begin = 0;
    while (begin < text.size() && is_space(text[begin])) {
        ++begin;
    }
    std::size_t end = text.size();
    while (end > begin && is_space(text[end - 1])) {
        --end;
    }
    return text.substr(begin, end - begin);
}

/// Nota que se muestra según cómo terminó la respuesta; vacía si fue normal.
std::string finish_note(std::string_view finish_reason) {
    if (finish_reason.empty() || finish_reason == "stop") {
        return {};
    }
    if (finish_reason == "length") {
        return "(cortada por límite de tokens)";
    }
    if (finish_reason == "content_filter") {
        return "(detenida por el filtro de contenido)";
    }
    return "(terminó por: " + std::string{finish_reason} + ")";
}

} // namespace

Conversation::Conversation() {
    history_.push_back(Message{Role::System, std::string{kSystemPrompt}});
}

std::optional<std::vector<Message>> Conversation::submit(std::string_view text) {
    const std::string_view trimmed = trim(text);
    if (trimmed.empty() || busy_) {
        return std::nullopt;
    }
    pending_user_text_ = std::string{trimmed};
    entries_.push_back(Entry{EntryKind::User, pending_user_text_, false, false, false, {}});
    history_.push_back(Message{Role::User, pending_user_text_});
    busy_ = true;
    return history_;
}

Entry* Conversation::current_answer() {
    if (entries_.empty()) {
        return nullptr;
    }
    Entry& last = entries_.back();
    return last.kind == EntryKind::Assistant && last.in_progress ? &last : nullptr;
}

void Conversation::append_delta(std::string_view text) {
    if (!busy_ || text.empty()) {
        return;
    }
    if (Entry* answer = current_answer()) {
        answer->text.append(text);
        return;
    }
    entries_.push_back(Entry{EntryKind::Assistant, std::string{text}, true, false, false, {}});
}

std::optional<std::string> Conversation::finish_success(std::string_view finish_reason) {
    if (!busy_) {
        return std::nullopt;
    }
    Entry* answer = current_answer();
    if (answer == nullptr || answer->text.empty()) {
        return finish_error(ChatError{ErrorKind::BadResponse, 0,
                                      "El modelo no devolvió texto.", std::nullopt});
    }
    answer->in_progress = false;
    answer->note = finish_note(finish_reason);
    history_.push_back(Message{Role::Assistant, answer->text});
    busy_ = false;
    return std::nullopt;
}

std::string Conversation::finish_error(const ChatError& error) {
    if (!busy_) {
        return {};
    }
    // El mensaje de usuario sale del historial para que no queden dos
    // mensajes de usuario seguidos; en pantalla se conserva.
    if (!history_.empty() && history_.back().role == Role::User) {
        history_.pop_back();
    }
    Entry* answer = current_answer();
    if (error.kind == ErrorKind::Cancelled) {
        // Cancelar no es un error: sin entrada en rojo.
        if (answer != nullptr) {
            answer->in_progress = false;
            answer->cancelled = true;
        } else {
            entries_.push_back(
                Entry{EntryKind::Notice, "Respuesta cancelada.", false, false, false, {}});
        }
    } else {
        if (answer != nullptr) {
            answer->in_progress = false;
            answer->incomplete = true;
        }
        entries_.push_back(Entry{EntryKind::Error,
                                 "[" + std::string{error_kind_label(error.kind)} + "] " +
                                     error.message,
                                 false, false, false, {}});
    }
    busy_ = false;
    return std::exchange(pending_user_text_, std::string{});
}

} // namespace chatbot::cli
