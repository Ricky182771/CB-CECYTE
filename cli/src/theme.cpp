#include "theme.h"

#include <ftxui/dom/node.hpp>
#include <ftxui/screen/box.hpp>
#include <ftxui/screen/screen.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <utility>

namespace chatbot::cli {

namespace {

using ftxui::Color;

// Tabla de temas. Agregar un tema es agregar una entrada aquí: nada más en
// el código depende de cuántos o cuáles hay. Las pruebas revisan el
// contraste de todo tema con fondo propio.
constexpr Theme kThemes[] = {
    // Catppuccin Mocha: https://github.com/catppuccin/catppuccin
    // Copyright (c) 2021 Catppuccin. Licencia MIT.
    {
        .id = "catppuccin-mocha",
        .name = "Catppuccin Mocha",
        .min_text_contrast = 4.5,
        .background = rgb(0x1E1E2E),
        .text = rgb(0xCDD6F4),
        .heading_accent = rgb(0x89B4FA),
        .inline_code = rgb(0xFAB387),
        .link = rgb(0x74C7EC),
        .highlight_fg = rgb(0x1E1E2E),
        .highlight_bg = rgb(0xF9E2AF),
        .user_label = rgb(0xA6E3A1),
        .assistant_label = rgb(0xCBA6F7),
        .error = rgb(0xF38BA8),
        .notice = rgb(0x9399B2),
        .border = rgb(0x6C7086),
        .table_border = rgb(0x6C7086),
        .quote_bar = rgb(0x6C7086),
        .alert_note = rgb(0x89B4FA),
        .alert_tip = rgb(0xA6E3A1),
        .alert_important = rgb(0xCBA6F7),
        .alert_warning = rgb(0xF9E2AF),
        .alert_caution = rgb(0xF38BA8),
        .selection_fg = rgb(0x1E1E2E),
        .selection_bg = rgb(0x89B4FA),
        .invert_selection = false,
        .input_placeholder = rgb(0x9399B2),
        .scrollbar = rgb(0x6C7086),
    },
    // Los colores y estilos de antes de los temas: paleta de 16 colores de la
    // terminal, dim e inverted, sin fondo ni color de texto propios.
    {
        .id = "terminal",
        .name = "De la terminal",
        .min_text_contrast = 0,
        .background = kNoColor,
        .text = kNoColor,
        .heading_accent = palette16(Color::Cyan),
        .inline_code = palette16(Color::Cyan),
        .link = kNoColor,
        .highlight_fg = palette16(Color::Black),
        .highlight_bg = palette16(Color::Yellow),
        .user_label = kNoColor,
        .assistant_label = kNoColor,
        .error = palette16(Color::Red),
        .notice = kDim,
        .border = kNoColor,
        .table_border = kDim,
        .quote_bar = kDim,
        .alert_note = kNoColor,
        .alert_tip = kNoColor,
        .alert_important = kNoColor,
        .alert_warning = kNoColor,
        .alert_caution = kNoColor,
        .selection_fg = kNoColor,
        .selection_bg = kNoColor,
        .invert_selection = true,
        .input_placeholder = kDim,
        .scrollbar = kNoColor,
    },
};

bool has_color(const ThemeColor& color) { return color.kind != ThemeColor::Kind::None; }

/// Pinta encima de lo que dibujaron los hijos: color de texto y fondo (y sin
/// dim) en todas las celdas visibles de su caja. A diferencia de
/// ftxui::color/bgcolor, que se aplican antes y los hijos pueden cambiar.
class PaintOver : public ftxui::Node {
public:
    PaintOver(ftxui::Element child, Color foreground, Color background)
        : ftxui::Node(ftxui::Elements{std::move(child)}), foreground_(foreground),
          background_(background) {}

    void ComputeRequirement() override {
        ftxui::Node::ComputeRequirement();
        requirement_ = children_[0]->requirement();
    }

    void SetBox(ftxui::Box box) override {
        ftxui::Node::SetBox(box);
        children_[0]->SetBox(box);
    }

    void Render(ftxui::Screen& screen) override {
        ftxui::Node::Render(screen);
        const ftxui::Box area = ftxui::Box::Intersection(box_, screen.stencil);
        for (int y = area.y_min; y <= area.y_max; ++y) {
            for (int x = area.x_min; x <= area.x_max; ++x) {
                ftxui::Cell& cell = screen.CellAt(x, y);
                cell.foreground_color = foreground_;
                cell.background_color = background_;
                cell.dim = false;
            }
        }
    }

private:
    Color foreground_;
    Color background_;
};

/// Asigna (no mezcla, como ftxui::color) el color de texto en toda su caja
/// antes de dibujar lo de adentro: así no hereda el color de un ancestro,
/// aunque el color sea Color::Default.
class ResetForeground : public ftxui::Node {
public:
    ResetForeground(ftxui::Element child, Color color)
        : ftxui::Node(ftxui::Elements{std::move(child)}), color_(color) {}

    void ComputeRequirement() override {
        ftxui::Node::ComputeRequirement();
        requirement_ = children_[0]->requirement();
    }

    void SetBox(ftxui::Box box) override {
        ftxui::Node::SetBox(box);
        children_[0]->SetBox(box);
    }

    void Render(ftxui::Screen& screen) override {
        const ftxui::Box area = ftxui::Box::Intersection(box_, screen.stencil);
        for (int y = area.y_min; y <= area.y_max; ++y) {
            for (int x = area.x_min; x <= area.x_max; ++x) {
                screen.CellAt(x, y).foreground_color = color_;
            }
        }
        ftxui::Node::Render(screen);
    }

private:
    Color color_;
};

/// ftxui::vscroll_indicator, y luego el color de la columna de la barra
/// (vscroll_indicator la dibuja en stencil.x_max sin tocar el color).
class ColoredScrollbar : public ftxui::Node {
public:
    ColoredScrollbar(ftxui::Element child, Color color)
        : ftxui::Node(ftxui::Elements{ftxui::vscroll_indicator(std::move(child))}),
          color_(color) {}

    void ComputeRequirement() override {
        ftxui::Node::ComputeRequirement();
        requirement_ = children_[0]->requirement();
    }

    void SetBox(ftxui::Box box) override {
        ftxui::Node::SetBox(box);
        children_[0]->SetBox(box);
    }

    void Render(ftxui::Screen& screen) override {
        ftxui::Node::Render(screen);
        const int x = screen.stencil.x_max;
        if (x < box_.x_min || x > box_.x_max) {
            return;
        }
        const int y_begin = std::max(box_.y_min, screen.stencil.y_min);
        const int y_end = std::min(box_.y_max, screen.stencil.y_max);
        for (int y = y_begin; y <= y_end; ++y) {
            screen.CellAt(x, y).foreground_color = color_;
        }
    }

private:
    Color color_;
};

double channel(std::uint32_t value) {
    const double c = static_cast<double>(value & 0xFFU) / 255.0;
    return c <= 0.03928 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

} // namespace

std::span<const Theme> themes() { return kThemes; }

const Theme& default_theme() { return kThemes[0]; }

const Theme* find_theme(std::string_view id) {
    for (const Theme& theme : kThemes) {
        if (theme.id == id) {
            return &theme;
        }
    }
    return nullptr;
}

bool has_own_background(const Theme& theme) { return has_color(theme.background); }

std::string_view background_id(BackgroundMode mode) {
    return mode == BackgroundMode::Transparent ? "terminal" : "theme";
}

std::optional<BackgroundMode> parse_background(std::string_view id) {
    if (id == "theme") {
        return BackgroundMode::FromTheme;
    }
    if (id == "terminal") {
        return BackgroundMode::Transparent;
    }
    return std::nullopt;
}

bool no_color_enabled() {
    const char* value = std::getenv("NO_COLOR");
    return value != nullptr && *value != '\0';
}

Appearance resolve_appearance(std::string_view theme_id, std::string_view background,
                              std::string* warning) {
    if (no_color_enabled()) {
        if (warning != nullptr) {
            warning->clear();
        }
        return Appearance{find_theme("terminal"), BackgroundMode::Transparent};
    }
    Appearance appearance;
    std::string notes;
    if (!theme_id.empty()) {
        if (const Theme* theme = find_theme(theme_id)) {
            appearance.theme = theme;
        } else {
            notes = "Tema desconocido en config.json («" + std::string{theme_id} +
                    "»): se usa " + std::string{default_theme().name} + ".";
        }
    }
    if (!background.empty()) {
        if (const std::optional<BackgroundMode> mode = parse_background(background)) {
            appearance.background = *mode;
        } else {
            notes += (notes.empty() ? "" : " ") + std::string{"Fondo desconocido en config.json («"} +
                     std::string{background} + "»): se usa el del tema.";
        }
    }
    if (warning != nullptr) {
        *warning = std::move(notes);
    }
    return appearance;
}

double relative_luminance(std::uint32_t color) {
    return 0.2126 * channel(color >> 16U) + 0.7152 * channel(color >> 8U) +
           0.0722 * channel(color);
}

double contrast_ratio(std::uint32_t a, std::uint32_t b) {
    const double la = relative_luminance(a);
    const double lb = relative_luminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

ftxui::Color to_ftxui(const ThemeColor& color) {
    switch (color.kind) {
    case ThemeColor::Kind::None:
        break;
    case ThemeColor::Kind::Palette16:
        return Color(static_cast<Color::Palette16>(color.value));
    case ThemeColor::Kind::Rgb:
        return Color::RGB(static_cast<std::uint8_t>(color.value >> 16U),
                          static_cast<std::uint8_t>(color.value >> 8U),
                          static_cast<std::uint8_t>(color.value));
    }
    return Color(Color::Default);
}

Palette::Palette(const Theme& theme, BackgroundMode background)
    : theme_(theme), background_(background) {
    if (background_ == BackgroundMode::Transparent) {
        theme_.background = kNoColor;
        theme_.text = kNoColor;
    }
}

bool Palette::paints_background() const { return has_color(theme_.background); }

ftxui::Decorator Palette::ink(ThemeColor Theme::*field) const {
    const ThemeColor color = theme_.*field;
    if (has_color(color) && color.dim) {
        return ftxui::color(to_ftxui(color)) | ftxui::dim;
    }
    if (has_color(color)) {
        return ftxui::color(to_ftxui(color));
    }
    if (color.dim) {
        return ftxui::dim;
    }
    return ftxui::nothing;
}

ftxui::Decorator Palette::base() const {
    ftxui::Decorator decorator = ftxui::nothing;
    if (has_color(theme_.text)) {
        decorator = ftxui::color(to_ftxui(theme_.text));
    }
    if (paints_background()) {
        decorator = decorator | ftxui::bgcolor(to_ftxui(theme_.background));
    }
    return decorator;
}

ftxui::Decorator Palette::selection() const {
    if (theme_.invert_selection) {
        return ftxui::inverted;
    }
    const Color foreground = to_ftxui(theme_.selection_fg);
    const Color background = to_ftxui(theme_.selection_bg);
    return [foreground, background](ftxui::Element element) -> ftxui::Element {
        return std::make_shared<PaintOver>(std::move(element), foreground, background);
    };
}

void Palette::ink_cell(ftxui::Cell& cell, ThemeColor Theme::*field) const {
    const ThemeColor color = theme_.*field;
    cell.foreground_color = to_ftxui(has_color(color) ? color : theme_.text);
    cell.dim = color.dim;
}

void Palette::select_cell(ftxui::Cell& cell) const {
    cell.dim = false;
    if (theme_.invert_selection) {
        cell.inverted = true;
        return;
    }
    cell.foreground_color = to_ftxui(theme_.selection_fg);
    cell.background_color = to_ftxui(theme_.selection_bg);
}

ftxui::Decorator Palette::highlight() const {
    // Como antes de los temas: el fondo afuera y el texto adentro.
    const ftxui::Decorator foreground = ink(&Theme::highlight_fg);
    const ThemeColor background = theme_.highlight_bg;
    if (!has_color(background)) {
        return foreground;
    }
    return foreground | ftxui::bgcolor(to_ftxui(background));
}

ftxui::Decorator Palette::inside_border() const {
    if (!has_color(theme_.border)) {
        return ftxui::nothing;
    }
    const Color text = to_ftxui(theme_.text);
    return [text](ftxui::Element element) -> ftxui::Element {
        return std::make_shared<ResetForeground>(std::move(element), text);
    };
}

ftxui::Element Palette::vscroll(ftxui::Element element) const {
    if (!has_color(theme_.scrollbar)) {
        return ftxui::vscroll_indicator(std::move(element));
    }
    return std::make_shared<ColoredScrollbar>(std::move(element), to_ftxui(theme_.scrollbar));
}

std::string Palette::key() const {
    return std::string{theme_.id} + "/" + std::string{background_id(background_)};
}

const Palette& terminal_palette() {
    static const Palette palette(*find_theme("terminal"), BackgroundMode::FromTheme);
    return palette;
}

} // namespace chatbot::cli
