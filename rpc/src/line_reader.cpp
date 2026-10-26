#include "line_reader.h"

#include <utility>

namespace chatbot::rpc {

namespace {

std::optional<LineReader::Item> complete_line(std::string text) {
    if (!text.empty() && text.back() == '\r') {
        text.pop_back();
    }
    if (text.empty()) {
        return std::nullopt;
    }
    return LineReader::Item{LineReader::Item::Kind::Line, std::move(text)};
}

} // namespace

std::vector<LineReader::Item> LineReader::feed(std::string_view bytes) {
    std::vector<Item> items;
    while (!bytes.empty()) {
        const std::size_t newline = bytes.find('\n');
        const std::string_view piece = bytes.substr(0, newline);
        if (!discarding_) {
            if (pending_.size() + piece.size() > max_bytes_) {
                pending_.clear();
                discarding_ = true;
                items.push_back(Item{Item::Kind::Oversized, {}});
            } else {
                pending_.append(piece);
            }
        }
        if (newline == std::string_view::npos) {
            break;
        }
        bytes.remove_prefix(newline + 1);
        if (discarding_) {
            discarding_ = false;
        } else if (std::optional<Item> line = complete_line(std::exchange(pending_, {}))) {
            items.push_back(std::move(*line));
        }
    }
    return items;
}

std::optional<LineReader::Item> LineReader::finish() {
    if (discarding_) {
        discarding_ = false;
        return std::nullopt;
    }
    return complete_line(std::exchange(pending_, {}));
}

} // namespace chatbot::rpc
