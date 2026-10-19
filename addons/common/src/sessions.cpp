#include "chatbot/bot/sessions.h"

#include <utility>

namespace chatbot::bot {

Session::Session(std::string_view system_prompt, TimePoint now) : last_activity(now) {
    if (!system_prompt.empty()) {
        history_.push_back(Message{Role::System, std::string{system_prompt}});
    }
}

std::vector<Message> Session::request(std::string user_content) const {
    std::vector<Message> messages = history_;
    messages.push_back(Message{Role::User, std::move(user_content)});
    return messages;
}

void Session::commit(std::string user_content, std::string answer) {
    history_.push_back(Message{Role::User, std::move(user_content)});
    history_.push_back(Message{Role::Assistant, std::move(answer)});
}

SessionStore::SessionStore(std::string system_prompt, std::chrono::minutes idle_timeout)
    : system_prompt_(std::move(system_prompt)), idle_timeout_(idle_timeout) {}

bool SessionStore::expired(const Session& session, TimePoint now) const {
    return !session.busy && now - session.last_activity >= idle_timeout_;
}

Session& SessionStore::get(ChatId chat, TimePoint now) {
    const auto found = sessions_.find(chat);
    if (found != sessions_.end()) {
        if (!expired(found->second, now)) {
            return found->second;
        }
        sessions_.erase(found);
    }
    return sessions_.emplace(chat, Session{system_prompt_, now}).first->second;
}

Session* SessionStore::find(ChatId chat) {
    const auto found = sessions_.find(chat);
    return found == sessions_.end() ? nullptr : &found->second;
}

bool SessionStore::reset(ChatId chat) {
    const auto found = sessions_.find(chat);
    if (found == sessions_.end()) {
        return true;
    }
    if (found->second.busy) {
        return false;
    }
    sessions_.erase(found);
    return true;
}

std::size_t SessionStore::expire(TimePoint now) {
    std::size_t removed = 0;
    for (auto it = sessions_.begin(); it != sessions_.end();) {
        if (expired(it->second, now)) {
            it = sessions_.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    return removed;
}

bool DailyCounter::try_count(UserId user, std::string_view day) {
    if (day != day_) {
        day_ = std::string{day};
        counts_.clear();
    }
    std::size_t& count = counts_[user];
    if (count >= limit_) {
        return false;
    }
    ++count;
    return true;
}

void DailyCounter::refund(UserId user, std::string_view day) {
    if (day != day_) {
        return;
    }
    const auto found = counts_.find(user);
    if (found != counts_.end() && found->second > 0) {
        --found->second;
    }
}

} // namespace chatbot::bot
