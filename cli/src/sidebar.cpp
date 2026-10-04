#include "sidebar.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <utility>

namespace chatbot::cli {

namespace {

std::chrono::sys_days to_days(CalendarDay day) {
    return std::chrono::sys_days{std::chrono::year{day.year} / std::chrono::month{day.month} /
                                 std::chrono::day{day.day}};
}

bool selectable(const SidebarRow& row) { return row.kind != SidebarRow::Kind::Header; }

} // namespace

std::optional<CalendarDay> local_day(std::time_t time) {
    std::tm local{};
    if (localtime_r(&time, &local) == nullptr) {
        return std::nullopt;
    }
    return CalendarDay{local.tm_year + 1900, static_cast<unsigned>(local.tm_mon + 1),
                       static_cast<unsigned>(local.tm_mday)};
}

std::optional<CalendarDay> day_of(std::string_view iso) {
    if (iso.size() < 10 || iso[4] != '-' || iso[7] != '-') {
        return std::nullopt;
    }
    const auto number = [iso](std::size_t at, std::size_t length) -> std::optional<int> {
        int value = 0;
        for (std::size_t i = at; i < at + length; ++i) {
            if (iso[i] < '0' || iso[i] > '9') {
                return std::nullopt;
            }
            value = value * 10 + (iso[i] - '0');
        }
        return value;
    };
    const std::optional<int> year = number(0, 4);
    const std::optional<int> month = number(5, 2);
    const std::optional<int> day = number(8, 2);
    if (!year || !month || !day) {
        return std::nullopt;
    }
    const CalendarDay result{*year, static_cast<unsigned>(*month), static_cast<unsigned>(*day)};
    const std::chrono::year_month_day check{std::chrono::year{result.year},
                                            std::chrono::month{result.month},
                                            std::chrono::day{result.day}};
    if (!check.ok()) {
        return std::nullopt;
    }
    return result;
}

std::string group_label(CalendarDay day, CalendarDay today) {
    const auto days_ago = (to_days(today) - to_days(day)).count();
    if (days_ago <= 0) {
        return "Hoy";
    }
    if (days_ago == 1) {
        return "Ayer";
    }
    if (days_ago <= 6) {
        return "Últimos 7 días";
    }
    static constexpr std::array<std::string_view, 12> kMonths{
        "ene", "feb", "mar", "abr", "may", "jun", "jul", "ago", "sep", "oct", "nov", "dic"};
    std::string label = std::to_string(day.day) + " " + std::string{kMonths[(day.month - 1) % 12]};
    if (day.year != today.year) {
        label += " " + std::to_string(day.year);
    }
    return label;
}

void Sidebar::open(std::vector<ConversationSummary> items, std::string current_id) {
    list_.open(std::move(items), std::move(current_id));
    new_selected_ = list_.empty() || !list_.is_current(list_.selected());
    top_ = 0;
}

void Sidebar::refresh(std::vector<ConversationSummary> items, std::string current_id) {
    list_.refresh(std::move(items));
    list_.set_current(std::move(current_id));
    if (list_.empty()) {
        new_selected_ = true;
    }
}

std::vector<SidebarRow> Sidebar::rows(CalendarDay today) const {
    std::vector<SidebarRow> rows;
    rows.push_back(SidebarRow{SidebarRow::Kind::New, {}, 0});
    std::string previous;
    for (std::size_t i = 0; i < list_.items().size(); ++i) {
        const ConversationSummary& item = list_.items()[i];
        std::string label;
        if (item.readable) {
            if (const std::optional<CalendarDay> day = day_of(item.updated_at)) {
                label = group_label(*day, today);
            }
        }
        if (label.empty()) {
            label = std::string{kUndated};
        }
        if (i == 0 || label != previous) {
            rows.push_back(SidebarRow{SidebarRow::Kind::Header, label, 0});
            previous = label;
        }
        rows.push_back(SidebarRow{SidebarRow::Kind::Conversation, {}, i});
    }
    return rows;
}

std::size_t Sidebar::selected_row(const std::vector<SidebarRow>& rows) const {
    if (!new_selected_) {
        for (std::size_t r = 0; r < rows.size(); ++r) {
            if (rows[r].kind == SidebarRow::Kind::Conversation &&
                rows[r].index == list_.selected()) {
                return r;
            }
        }
    }
    return 0; // "+ Nueva".
}

ListAction Sidebar::handle(ListKey key, std::string_view character, std::size_t page_size) {
    // Con una confirmación pendiente, la lista decide ('s' borra; lo demás
    // cancela). Esc sin confirmación devuelve el foco.
    if (list_.confirmation().has_value() || key == ListKey::Escape) {
        return list_.handle(key, character, page_size);
    }
    if (list_.empty()) {
        new_selected_ = true;
    }
    if (new_selected_) {
        const std::size_t last = list_.empty() ? 0 : list_.items().size() - 1;
        const std::size_t page = std::max<std::size_t>(page_size, 1);
        switch (key) {
        case ListKey::Enter:
            return ListAction{ListAction::Type::New, {}};
        case ListKey::Down:
        case ListKey::PageDown:
        case ListKey::Home:
        case ListKey::End:
            if (!list_.empty()) {
                new_selected_ = false;
                if (key == ListKey::PageDown) {
                    list_.select(std::min(page - 1, last));
                } else {
                    list_.select(key == ListKey::End ? last : 0);
                }
            }
            return {};
        default:
            return {}; // ↑, PgUp, Supr y el resto no hacen nada aquí.
        }
    }
    // Desde la primera conversación, ↑ y PgUp suben a "+ Nueva".
    if ((key == ListKey::Up || key == ListKey::PageUp) && list_.selected() == 0) {
        new_selected_ = true;
        return {};
    }
    return list_.handle(key, character, page_size);
}

void Sidebar::fit(const std::vector<SidebarRow>& rows, std::size_t view_height) {
    const std::size_t height = std::max<std::size_t>(view_height, 1);
    const std::size_t max_top = rows.size() > height ? rows.size() - height : 0;
    top_ = std::min(top_, max_top);
    const std::size_t selected = selected_row(rows);
    if (selected < top_) {
        // Si la selección encabeza su grupo, también se ve el encabezado.
        const bool header_above =
            selected > 0 && rows[selected - 1].kind == SidebarRow::Kind::Header;
        top_ = header_above ? selected - 1 : selected;
    } else if (selected >= top_ + height) {
        top_ = selected - height + 1;
    }
}

void Sidebar::scroll(int lines, const std::vector<SidebarRow>& rows, std::size_t view_height) {
    const std::size_t height = std::max<std::size_t>(view_height, 1);
    const std::size_t max_top = rows.size() > height ? rows.size() - height : 0;
    if (lines < 0) {
        const auto up = static_cast<std::size_t>(-static_cast<long long>(lines));
        top_ = top_ > up ? top_ - up : 0;
    } else {
        top_ = std::min(top_ + static_cast<std::size_t>(lines), max_top);
    }
    const std::size_t end = std::min(top_ + height, rows.size());
    const std::size_t selected = selected_row(rows);
    if (selected < top_) {
        for (std::size_t r = top_; r < end; ++r) {
            if (selectable(rows[r])) {
                select_row(rows[r]);
                break;
            }
        }
    } else if (selected >= end) {
        for (std::size_t r = end; r > top_; --r) {
            if (selectable(rows[r - 1])) {
                select_row(rows[r - 1]);
                break;
            }
        }
    }
}

ListAction Sidebar::click(std::size_t line, const std::vector<SidebarRow>& rows) {
    const std::size_t r = top_ + line;
    if (r >= rows.size() || !selectable(rows[r])) {
        return {};
    }
    select_row(rows[r]);
    if (rows[r].kind == SidebarRow::Kind::New) {
        return ListAction{ListAction::Type::New, {}};
    }
    if (!list_.can_open(rows[r].index)) {
        return {};
    }
    return ListAction{ListAction::Type::Open, list_.items()[rows[r].index].id};
}

void Sidebar::select_row(const SidebarRow& row) {
    if (row.kind == SidebarRow::Kind::New) {
        new_selected_ = true;
        list_.select(list_.selected()); // Cancela una confirmación pendiente.
    } else if (row.kind == SidebarRow::Kind::Conversation) {
        new_selected_ = false;
        list_.select(row.index);
    }
}

} // namespace chatbot::cli
