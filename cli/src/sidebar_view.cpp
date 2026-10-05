#include "sidebar_view.h"

#include "markdown.h"

#include <ftxui/screen/string.hpp>

#include <algorithm>
#include <cstddef>
#include <utility>

namespace chatbot::cli {

namespace {

constexpr std::string_view kTitle = " Conversaciones ";
constexpr std::string_view kNew = "+ Nueva";
constexpr std::string_view kNewKey = "  (Ctrl+N)";
constexpr std::string_view kSettings = "⚙ Configuración";
constexpr std::string_view kSettingsKey = "  (F2)";
constexpr std::string_view kUnreadable = " (ilegible)";

std::string repeat(std::string_view piece, int count) {
    std::string out;
    for (int i = 0; i < count; ++i) {
        out += piece;
    }
    return out;
}

/// Título en una sola línea y sin controles de terminal.
std::string clean_title(const std::string& title) {
    std::string text = md::sanitize(title);
    std::replace(text.begin(), text.end(), '\n', ' ');
    return text;
}

/// Contenido de una fila, de content_width columnas (con relleno). Cada
/// tramo lleva su estilo; el relleno, ninguno.
ftxui::Element row_content(const Sidebar& sidebar, const SidebarRow& row, int content_width) {
    ftxui::Elements parts;
    int used = 0;
    const auto add = [&](const std::string& text, const ftxui::Decorator& style) {
        if (text.empty()) {
            return;
        }
        used += ftxui::string_width(text);
        parts.push_back(style ? style(ftxui::text(text)) : ftxui::text(text));
    };
    switch (row.kind) {
    case SidebarRow::Kind::New:
    case SidebarRow::Kind::Settings: {
        // La tecla, tenue, solo si cabe entera.
        const bool is_new = row.kind == SidebarRow::Kind::New;
        const std::string_view label = is_new ? kNew : kSettings;
        const std::string_view key = is_new ? kNewKey : kSettingsKey;
        if (ftxui::string_width(std::string{label} + std::string{key}) <= content_width) {
            add(std::string{label}, {});
            add(std::string{key}, ftxui::dim);
        } else {
            add(fit_width(label, content_width), {});
        }
        break;
    }
    case SidebarRow::Kind::Header:
        add(fit_width(row.text, content_width), ftxui::bold);
        break;
    case SidebarRow::Kind::Conversation: {
        const ConversationSummary& item = sidebar.list().items()[row.index];
        const std::string marker = sidebar.list().is_current(row.index) ? "● " : "  ";
        const int title_width = content_width - ftxui::string_width(marker);
        add(marker, {});
        const std::string title = clean_title(item.title);
        if (item.readable) {
            add(fit_width(title, title_width), {});
        } else {
            // Las ilegibles van tenues y con "(ilegible)", que no se recorta
            // mientras quepa algo del nombre.
            const int name_width = title_width - ftxui::string_width(kUnreadable);
            if (name_width >= 2) {
                add(fit_width(title, name_width) + std::string{kUnreadable}, ftxui::dim);
            } else {
                add(fit_width(title + std::string{kUnreadable}, title_width), ftxui::dim);
            }
        }
        break;
    }
    }
    if (used < content_width) {
        parts.push_back(ftxui::text(std::string(static_cast<std::size_t>(content_width - used), ' ')));
    }
    return ftxui::hbox(std::move(parts));
}

} // namespace

std::string fit_width(std::string_view text, int width) {
    if (width <= 0) {
        return {};
    }
    if (ftxui::string_width(text) <= width) {
        return std::string{text};
    }
    static const std::string kEllipsis = "…";
    const int room = width - ftxui::string_width(kEllipsis);
    std::string out;
    int used = 0;
    for (const std::string& glyph : ftxui::Utf8ToGlyphs(text)) {
        if (glyph.empty()) {
            continue; // Segunda celda de un carácter ancho.
        }
        const int glyph_width = ftxui::string_width(glyph);
        if (used + glyph_width > room) {
            break;
        }
        out += glyph;
        used += glyph_width;
    }
    return out + kEllipsis;
}

int sidebar_view_height(int height) { return std::max(height - 2, 1); }

ftxui::Element render_sidebar(const Sidebar& sidebar, const std::vector<SidebarRow>& rows,
                              int width, int height, bool focused) {
    width = std::max(width, 4);
    const int view = sidebar_view_height(height);
    // "│ " + contenido + " ": el borde derecho lo pone sidebar_divider().
    const int content_width = width - 3;

    ftxui::Elements lines;
    // Arriba: "┌ Conversaciones ───…".
    const std::string title = fit_width(kTitle, width - 1);
    lines.push_back(ftxui::hbox({
        ftxui::text("┌"),
        ftxui::text(title) | ftxui::bold,
        ftxui::text(repeat("─", width - 1 - ftxui::string_width(title))),
    }));

    const std::size_t selected = sidebar.selected_row(rows);
    for (int line = 0; line < view; ++line) {
        const std::size_t r = sidebar.top() + static_cast<std::size_t>(line);
        ftxui::Element content;
        if (r < rows.size()) {
            content = row_content(sidebar, rows[r], content_width);
            if (focused && r == selected) {
                content = content | ftxui::inverted; // Fila seleccionada, a todo lo ancho.
            }
        } else if (sidebar.list().empty() && r == rows.size()) {
            const std::string empty =
                fit_width(ConversationList::kEmptyMessage, content_width);
            content = ftxui::hbox({
                ftxui::text(empty) | ftxui::dim,
                ftxui::text(std::string(
                    static_cast<std::size_t>(content_width - ftxui::string_width(empty)), ' ')),
            });
        } else {
            content = ftxui::text(std::string(static_cast<std::size_t>(content_width), ' '));
        }
        lines.push_back(ftxui::hbox({ftxui::text("│ "), std::move(content), ftxui::text(" ")}));
    }

    lines.push_back(ftxui::text("└" + repeat("─", width - 1)));
    return ftxui::vbox(std::move(lines));
}

ftxui::Element sidebar_divider() {
    return ftxui::vbox({ftxui::text("┐"), ftxui::separatorLight() | ftxui::yflex,
                        ftxui::text("┘")});
}

} // namespace chatbot::cli
