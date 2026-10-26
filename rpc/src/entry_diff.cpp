#include "entry_diff.h"

namespace chatbot::rpc {

EntriesSeen remember(const std::vector<cli::Entry>& entries) {
    EntriesSeen seen;
    seen.count = entries.size();
    if (!entries.empty()) {
        const cli::Entry& last = entries.back();
        seen.last_text_size = last.text.size();
        seen.last_in_progress = last.in_progress;
        seen.last_incomplete = last.incomplete;
        seen.last_cancelled = last.cancelled;
        seen.last_note = last.note;
    }
    return seen;
}

EntriesDiff diff_entries(const EntriesSeen& seen, const std::vector<cli::Entry>& entries) {
    EntriesDiff diff;
    diff.added_end = entries.size();
    if (seen.count == 0 || seen.count > entries.size()) {
        return diff;
    }
    const cli::Entry& last = entries[seen.count - 1];
    diff.grown = last.text.size() > seen.last_text_size;
    diff.updated = last.in_progress != seen.last_in_progress ||
                   last.incomplete != seen.last_incomplete ||
                   last.cancelled != seen.last_cancelled || last.note != seen.last_note;
    return diff;
}

} // namespace chatbot::rpc
