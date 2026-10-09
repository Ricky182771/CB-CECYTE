#include "conversation.h"

#include <cctype>
#include <ctime>
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

Conversation::Conversation(std::string_view system_prompt) {
    (void)set_system_prompt(system_prompt);
}

bool Conversation::set_system_prompt(std::string_view system_prompt) {
    if (busy_) {
        return false;
    }
    const bool has_system = !history_.empty() && history_.front().role == Role::System;
    if (system_prompt.empty()) {
        if (has_system) {
            history_.erase(history_.begin());
        }
    } else if (has_system) {
        history_.front().content = std::string{system_prompt};
    } else {
        history_.insert(history_.begin(), Message{Role::System, std::string{system_prompt}});
    }
    return true;
}

std::optional<std::vector<Message>> Conversation::submit(std::string_view text) {
    const std::string_view trimmed = trim(text);
    if (trimmed.empty() || busy_) {
        return std::nullopt;
    }
    pending_user_text_ = std::string{trimmed};
    entries_.push_back(Entry{EntryKind::User, pending_user_text_, false, false, false, {}, {}});
    history_.push_back(Message{Role::User, pending_user_text_});
    busy_ = true;
    return history_;
}

bool Conversation::attach_search(SearchResponse response, std::string date, std::string block) {
    if (!busy_) {
        return false;
    }
    if (!history_.empty() && history_.back().role == Role::User) {
        history_.back().content = std::move(block);
    }
    pending_search_ = StoredSearch{std::move(date), std::move(response)};
    return true;
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
    entries_.push_back(Entry{EntryKind::Assistant, std::string{text}, true, false, false, {}, {}});
}

std::optional<std::string> Conversation::finish_success(std::string_view finish_reason,
                                                        std::string_view model) {
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
    const std::string answer_text = answer->text;
    if (pending_search_.has_value()) {
        Entry sources{EntryKind::Sources, {}, false, false, false, {}, {}};
        sources.sources = pending_search_->response.results;
        entries_.push_back(std::move(sources)); // answer deja de ser válido.
    }
    // Par terminado: es lo único que se guarda.
    turns_.push_back(
        StoredMessage{Role::User, pending_user_text_, {}, {}, std::move(pending_search_)});
    pending_search_.reset();
    turns_.push_back(StoredMessage{Role::Assistant, answer_text, std::string{model},
                                   std::string{finish_reason}, std::nullopt});
    if (title_.empty()) {
        title_ = make_title(turns_.front().content);
    }
    pending_user_text_.clear();
    busy_ = false;
    return std::nullopt;
}

void Conversation::add_error(std::string text) {
    entries_.push_back(Entry{EntryKind::Error, std::move(text), false, false, false, {}, {}});
}

void Conversation::add_notice(std::string text) {
    entries_.push_back(Entry{EntryKind::Notice, std::move(text), false, false, false, {}, {}});
}

void Conversation::set_identity(std::string id, std::string created_at) {
    id_ = std::move(id);
    created_at_ = std::move(created_at);
}

std::string Conversation::last_model() const {
    for (auto turn = turns_.rbegin(); turn != turns_.rend(); ++turn) {
        if (turn->role == Role::Assistant && !turn->model.empty()) {
            return turn->model;
        }
    }
    return {};
}

StoredConversation Conversation::to_stored(std::string updated_at) const {
    return StoredConversation{id_, title_, created_at_, std::move(updated_at), turns_};
}

Conversation Conversation::from_stored(const StoredConversation& stored,
                                       std::string_view system_prompt,
                                       const std::function<std::string()>& make_nonce) {
    Conversation conversation{system_prompt}; // El historial guardado no lo trae.
    conversation.id_ = stored.id;
    conversation.created_at_ = stored.created_at;
    conversation.title_ = stored.title;
    conversation.turns_ = stored.messages;
    // Fuentes de la última búsqueda: van después de su respuesta.
    std::optional<std::vector<SearchResult>> sources;
    const auto flush_sources = [&conversation, &sources] {
        if (sources.has_value()) {
            Entry entry{EntryKind::Sources, {}, false, false, false, {}, {}};
            entry.sources = std::move(*sources);
            conversation.entries_.push_back(std::move(entry));
            sources.reset();
        }
    };
    for (const StoredMessage& message : stored.messages) {
        if (message.role == Role::Assistant) {
            conversation.history_.push_back(Message{message.role, message.content});
            conversation.entries_.push_back(Entry{EntryKind::Assistant, message.content, false,
                                                  false, false,
                                                  finish_note(message.finish_reason), {}});
            flush_sources();
            continue;
        }
        flush_sources(); // Un User sin respuesta en medio (archivo editado a mano).
        std::string content = message.content;
        if (message.search.has_value()) {
            // Sin "date" (archivo editado a mano), la fecha de la conversación.
            const std::string& date =
                message.search->date.empty() ? stored.created_at : message.search->date;
            content = format_search_context(message.search->response, spanish_date(date),
                                            make_nonce());
            sources = message.search->response.results;
        }
        conversation.history_.push_back(Message{message.role, std::move(content)});
        conversation.entries_.push_back(
            Entry{EntryKind::User, message.content, false, false, false, {}, {}});
    }
    flush_sources();
    return conversation;
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
    pending_search_.reset();
    Entry* answer = current_answer();
    if (error.kind == ErrorKind::BadResponse && error.message == kNoSearchResults) {
        // No se llamó al modelo: no hay respuesta parcial ni es un error.
        entries_.push_back(
            Entry{EntryKind::Notice, error.message, false, false, false, {}, {}});
    } else if (error.kind == ErrorKind::Cancelled) {
        // Cancelar no es un error: sin entrada en rojo.
        if (answer != nullptr) {
            answer->in_progress = false;
            answer->cancelled = true;
        } else {
            entries_.push_back(
                Entry{EntryKind::Notice, "Respuesta cancelada.", false, false, false, {}, {}});
        }
    } else {
        if (answer != nullptr) {
            answer->in_progress = false;
            answer->incomplete = true;
        }
        entries_.push_back(Entry{EntryKind::Error,
                                 "[" + std::string{error_kind_label(error.kind)} + "] " +
                                     error.message,
                                 false, false, false, {}, {}});
    }
    busy_ = false;
    return std::exchange(pending_user_text_, std::string{});
}

void save_conversation(Conversation& conversation, const ConversationStore& store) {
    if (!conversation.has_turns()) {
        return;
    }
    const std::string now = format_iso8601(std::time(nullptr));
    if (conversation.id().empty()) {
        conversation.set_identity(store.new_id(), now);
    }
    if (const std::optional<std::string> error = store.save(conversation.to_stored(now))) {
        conversation.add_error("No se pudo guardar la conversación: " + *error);
    }
}

} // namespace chatbot::cli
