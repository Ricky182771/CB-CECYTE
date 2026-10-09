#include "status_line.h"

#include <ftxui/screen/string.hpp>

#include <algorithm>
#include <cstddef>
#include <string_view>
#include <utility>

namespace chatbot::cli {

namespace {

constexpr std::string_view kSeparator = "   ";
constexpr int kSeparatorWidth = 3;

int width_of(const std::string& text) { return ftxui::string_width(text); }

/// text recortado a width columnas con "…" al final, sin partir glifos ni
/// dejar espacios antes del "…". Vacío si no quedan al menos 2 columnas (un
/// "…" solo no dice nada).
std::string shortened(const std::string& text, int width) {
    if (width < 2) {
        return {};
    }
    std::string out;
    int used = 0;
    for (const std::string& glyph : ftxui::Utf8ToGlyphs(text)) {
        const int glyph_width = ftxui::string_width(glyph);
        if (used + glyph_width > width - 1) {
            break;
        }
        out += glyph;
        used += glyph_width;
    }
    out.erase(out.find_last_not_of(' ') + 1);
    return out + "…";
}

} // namespace

ftxui::Element status_line(const std::vector<StatusItem>& items, int width) {
    width = std::max(width, 1);
    // Indicadores que caben, desde el primero; el primero siempre.
    std::vector<bool> shown(items.size(), false);
    int reserved = 0;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (!items[i].reserved) {
            continue;
        }
        const int needed = width_of(items[i].text) + (reserved > 0 ? kSeparatorWidth : 0);
        if (reserved == 0 || reserved + needed <= width) {
            shown[i] = true;
            reserved += needed;
        }
    }
    // Avisos con lo que sobra.
    std::vector<std::string> texts(items.size());
    int used = reserved;
    bool full = false;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (items[i].reserved) {
            texts[i] = items[i].text;
            continue;
        }
        if (full || items[i].text.empty()) {
            continue;
        }
        const int separator = used > 0 ? kSeparatorWidth : 0;
        const int room = width - used - separator;
        const int needed = width_of(items[i].text);
        if (needed <= room) {
            texts[i] = items[i].text;
        } else {
            texts[i] = shortened(items[i].text, room);
            full = true;
        }
        if (!texts[i].empty()) {
            shown[i] = true;
            used += separator + width_of(texts[i]);
        }
    }
    ftxui::Elements row;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (!shown[i]) {
            continue;
        }
        if (!row.empty()) {
            row.push_back(ftxui::text(std::string{kSeparator}));
        }
        ftxui::Element part = ftxui::text(texts[i]);
        row.push_back(items[i].style ? std::move(part) | items[i].style : std::move(part));
    }
    if (row.empty()) {
        // Alto fijo: sin esto, un hbox vacío mide 0 líneas y la pantalla salta.
        row.push_back(ftxui::text(""));
    }
    return ftxui::hbox(std::move(row));
}

} // namespace chatbot::cli
