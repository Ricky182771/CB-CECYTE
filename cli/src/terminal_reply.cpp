#include "terminal_reply.h"

namespace chatbot::cli {

namespace {

/// ESC [ ? … c o ESC [ > … c, con dígitos y ';' en medio.
bool is_device_attributes(std::string_view input) {
    if (input.size() < 4 || !input.starts_with("\x1B[") || input.back() != 'c' ||
        (input[2] != '?' && input[2] != '>')) {
        return false;
    }
    for (const char c : input.substr(3, input.size() - 4)) {
        if ((c < '0' || c > '9') && c != ';') {
            return false;
        }
    }
    return true;
}

} // namespace

bool is_terminal_reply(std::string_view input) {
    // ESC P y ESC \ no se pueden encimar: al menos 4 bytes.
    const bool dcs = input.size() >= 4 && input.starts_with("\x1BP") && input.ends_with("\x1B\\");
    return dcs || is_device_attributes(input);
}

} // namespace chatbot::cli
