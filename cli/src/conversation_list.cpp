#include "conversation_list.h"

#include <algorithm>
#include <utility>

namespace chatbot::cli {

void ConversationList::open(std::vector<ConversationSummary> items, std::string current_id) {
    items_ = std::move(items);
    current_id_ = std::move(current_id);
    confirming_delete_ = false;
    selected_ = 0;
    for (std::size_t i = 0; i < items_.size(); ++i) {
        if (!current_id_.empty() && items_[i].id == current_id_) {
            selected_ = i;
            break;
        }
    }
}

void ConversationList::refresh(std::vector<ConversationSummary> items) {
    const std::string previous_id = selected_ < items_.size() ? items_[selected_].id : std::string{};
    const std::size_t previous_index = selected_;
    items_ = std::move(items);
    confirming_delete_ = false;
    selected_ = previous_index;
    for (std::size_t i = 0; i < items_.size(); ++i) {
        if (!previous_id.empty() && items_[i].id == previous_id) {
            selected_ = i;
            break;
        }
    }
    selected_ = items_.empty() ? 0 : std::min(selected_, items_.size() - 1);
}

bool ConversationList::is_current(std::size_t index) const {
    return index < items_.size() && !current_id_.empty() && items_[index].id == current_id_;
}

bool ConversationList::can_open(std::size_t index) const {
    return index < items_.size() && items_[index].readable;
}

std::optional<std::string> ConversationList::confirmation() const {
    if (!confirming_delete_ || selected_ >= items_.size()) {
        return std::nullopt;
    }
    return "¿Borrar «" + items_[selected_].title + "»? (s/n)";
}

ListAction ConversationList::handle(ListKey key, std::string_view character,
                                    std::size_t page_size) {
    if (confirming_delete_) {
        // Solo 's' borra; cualquier otra tecla cancela y no hace nada más.
        confirming_delete_ = false;
        if (key == ListKey::Other && character == "s" && can_open(selected_)) {
            return ListAction{ListAction::Type::Delete, items_[selected_].id};
        }
        return {};
    }
    if (key == ListKey::Escape) {
        return ListAction{ListAction::Type::Close, {}};
    }
    if (items_.empty()) {
        return {};
    }
    const std::size_t last = items_.size() - 1;
    const std::size_t page = std::max<std::size_t>(page_size, 1);
    switch (key) {
    case ListKey::Up:
        selected_ = selected_ > 0 ? selected_ - 1 : 0;
        break;
    case ListKey::Down:
        selected_ = std::min(selected_ + 1, last);
        break;
    case ListKey::PageUp:
        selected_ = selected_ > page ? selected_ - page : 0;
        break;
    case ListKey::PageDown:
        selected_ = std::min(selected_ + page, last);
        break;
    case ListKey::Home:
        selected_ = 0;
        break;
    case ListKey::End:
        selected_ = last;
        break;
    case ListKey::Enter:
        if (can_open(selected_)) {
            return ListAction{ListAction::Type::Open, items_[selected_].id};
        }
        break;
    case ListKey::Delete:
        confirming_delete_ = can_open(selected_);
        break;
    case ListKey::Escape:
    case ListKey::Other:
        break;
    }
    return {};
}

std::string ConversationList::format_date(const std::string& iso) {
    if (iso.size() >= 16 && iso[10] == 'T') {
        return iso.substr(0, 10) + " " + iso.substr(11, 5);
    }
    return iso;
}

std::string ConversationList::details(const ConversationSummary& summary) {
    if (!summary.readable) {
        return "ilegible: " + summary.error;
    }
    std::string text = format_date(summary.updated_at) + " · " +
                       std::to_string(summary.message_count) +
                       (summary.message_count == 1 ? " mensaje" : " mensajes");
    if (!summary.last_model.empty()) {
        text += " · " + summary.last_model;
    }
    return text;
}

} // namespace chatbot::cli
