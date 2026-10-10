#include "paste.h"

#include "chatbot/utf8.h"

#include <cstdint>

namespace chatbot::cli {

namespace {

/// Espacios que reemplazan a un tabulador (igual que md::sanitize).
constexpr std::string_view kTabSpaces = "    ";

/// Agrega piece si cabe en kMaxPasteBytes; si no, marca el recorte.
bool append_bounded(SanitizedPaste& out, std::string_view piece) {
    if (out.text.size() + piece.size() > kMaxPasteBytes) {
        out.truncated = true;
        return false;
    }
    out.text += piece;
    return true;
}

/// Una sola línea: fuera los "\n" de los extremos; los de en medio, espacio.
void to_single_line(std::string& text) {
    const std::size_t begin = text.find_first_not_of('\n');
    if (begin == std::string::npos) {
        text.clear();
        return;
    }
    const std::size_t end = text.find_last_not_of('\n');
    text = text.substr(begin, end - begin + 1);
    for (char& c : text) {
        if (c == '\n') {
            c = ' ';
        }
    }
}

} // namespace

SanitizedPaste sanitize_paste(std::string_view text, bool multiline) {
    SanitizedPaste out;
    std::size_t i = 0;
    while (i < text.size()) {
        const std::size_t start = i;
        const std::optional<std::uint32_t> code = utf8::next_code_point(text, i);
        std::string_view piece;
        if (!code.has_value()) {
            ++i; // Cada byte inválido es un U+FFFD.
            piece = utf8::kReplacement;
        } else if (*code == '\r') {
            if (i < text.size() && text[i] == '\n') {
                ++i; // "\r\n" es un solo salto.
            }
            piece = "\n";
        } else if (*code == '\n') {
            piece = "\n";
        } else if (*code == '\t') {
            piece = kTabSpaces;
        } else if (*code < 0x20 || *code == 0x7F || (*code >= 0x80 && *code <= 0x9F)) {
            continue; // Controles C0, DEL y C1.
        } else {
            piece = text.substr(start, i - start);
        }
        if (!append_bounded(out, piece)) {
            break;
        }
    }
    if (!multiline) {
        to_single_line(out.text);
    }
    return out;
}

PasteTarget paste_target(bool sidebar_focused, bool settings_open, PasteTarget settings_field) {
    if (sidebar_focused) {
        return PasteTarget::None;
    }
    return settings_open ? settings_field : PasteTarget::MultiLine;
}

std::optional<std::string_view> paste_shortcut_notice(bool sidebar_focused, PasteTarget target) {
    if (target != PasteTarget::None || sidebar_focused) {
        return std::nullopt;
    }
    return kPasteNoTarget;
}

std::optional<SanitizedPaste> prepare_paste(std::string_view raw, PasteTarget target) {
    if (target == PasteTarget::None) {
        return std::nullopt;
    }
    SanitizedPaste paste = sanitize_paste(raw, target == PasteTarget::MultiLine);
    if (paste.text.empty()) {
        return std::nullopt;
    }
    return paste;
}

void BracketedPaste::append(std::string_view piece) {
    if (truncated_) {
        return;
    }
    if (buffer_.size() + piece.size() > kMaxPasteBytes) {
        truncated_ = true; // Lo demás se descarta hasta la marca de fin.
        return;
    }
    buffer_ += piece;
}

BracketedPaste::Step BracketedPaste::finish() {
    Step step{true, std::move(buffer_), truncated_};
    buffer_.clear();
    truncated_ = false;
    active_ = false;
    return step;
}

BracketedPaste::Step BracketedPaste::feed(PasteKey key, std::string_view character) {
    const std::chrono::steady_clock::time_point now = clock_();
    // Un pegado sin fin después de una pausa: se entrega y el evento sigue
    // como si no hubiera pegado.
    Step closed;
    if (active_ && now - last_ > kPasteIdleLimit) {
        closed = finish();
        closed.consumed = false;
    }
    Step step = process(key, character);
    if (active_ && step.consumed) {
        last_ = now;
    }
    if (closed.text.has_value()) {
        // Sin pegado activo, process nunca entrega texto.
        step.text = std::move(closed.text);
        step.truncated = closed.truncated;
    }
    return step;
}

BracketedPaste::Step BracketedPaste::process(PasteKey key, std::string_view character) {
    switch (key) {
    case PasteKey::Passthrough:
        return {};
    case PasteKey::Start: {
        // Un Start sin End antes: lo que había también se pegó.
        Step step = active_ ? finish() : Step{true, std::nullopt, false};
        active_ = true;
        return step;
    }
    case PasteKey::End:
        return active_ ? finish() : Step{true, std::nullopt, false};
    case PasteKey::Character:
    case PasteKey::Return:
    case PasteKey::Tab:
    case PasteKey::Other:
        break;
    }
    if (!active_) {
        return {};
    }
    if (key == PasteKey::Character) {
        append(character);
    } else if (key == PasteKey::Return) {
        append("\n");
    } else if (key == PasteKey::Tab) {
        append("\t");
    }
    return Step{true, std::nullopt, false};
}

} // namespace chatbot::cli
