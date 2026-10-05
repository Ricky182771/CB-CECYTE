#ifndef CHATBOT_CLI_THEME_H
#define CHATBOT_CLI_THEME_H

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace chatbot::cli {

/// Color de un uso del tema, como datos: ninguno (el de la terminal), uno de
/// la paleta de 16 colores de la terminal o RGB. dim: además, tenue.
struct ThemeColor {
    enum class Kind : std::uint8_t { None, Palette16, Rgb };
    Kind kind = Kind::None;
    std::uint32_t value = 0; ///< 0xRRGGBB, o el índice de la paleta de 16.
    bool dim = false;
};

[[nodiscard]] constexpr ThemeColor rgb(std::uint32_t hex) {
    return ThemeColor{ThemeColor::Kind::Rgb, hex, false};
}
[[nodiscard]] constexpr ThemeColor palette16(ftxui::Color::Palette16 index) {
    return ThemeColor{ThemeColor::Kind::Palette16, index, false};
}
/// Sin color propio: el de la terminal (o el que herede).
inline constexpr ThemeColor kNoColor{};
/// Sin color propio y tenue.
inline constexpr ThemeColor kDim{ThemeColor::Kind::None, 0, true};

/// Un tema: un campo por uso, no por color. Los temas son datos (ver
/// themes()); agregar uno es agregar una entrada a la tabla de theme.cpp.
struct Theme {
    std::string_view id;   ///< Lo que se guarda en config.json.
    std::string_view name; ///< Nombre visible.
    /// Contraste mínimo (WCAG 2.x) de cada color de texto contra el fondo.
    /// Solo lo revisan las pruebas, y solo en temas con fondo propio.
    double min_text_contrast = 0;

    ThemeColor background; ///< Fondo de toda la pantalla (None: el de la terminal).
    ThemeColor text;       ///< Texto normal.
    ThemeColor heading_accent; ///< Subtítulos H3-H6 y la marca "●" del elegido.
    ThemeColor inline_code;    ///< Código en línea y LaTeX.
    ThemeColor link;           ///< Texto de un enlace (además va subrayado).
    ThemeColor highlight_fg;   ///< ==resaltado==.
    ThemeColor highlight_bg;
    ThemeColor user_label;      ///< "Tú:" y el "> " de la caja.
    ThemeColor assistant_label; ///< "Asistente:".
    ThemeColor error;
    /// Texto secundario: avisos, notas, URL entre paréntesis, ayuda de teclas.
    ThemeColor notice;
    ThemeColor border;       ///< Marcos y separadores.
    ThemeColor table_border; ///< Líneas de las tablas y separadores de tarjetas.
    ThemeColor quote_bar;    ///< Barra "│" de las citas (y de las alertas sin color).
    ThemeColor alert_note;
    ThemeColor alert_tip;
    ThemeColor alert_important;
    ThemeColor alert_warning;
    ThemeColor alert_caution;
    ThemeColor selection_fg; ///< Cursor de una lista, campo o botón con el foco.
    ThemeColor selection_bg;
    /// La selección invierte el texto en vez de usar selection_fg/_bg.
    bool invert_selection = false;
    ThemeColor input_placeholder;
    ThemeColor scrollbar;
};

/// Todos los temas, en el orden en que se muestran. El primero es el de
/// por defecto.
[[nodiscard]] std::span<const Theme> themes();
[[nodiscard]] const Theme& default_theme();
/// El tema con ese id, o nullptr.
[[nodiscard]] const Theme* find_theme(std::string_view id);
/// true si el tema pinta su propio fondo.
[[nodiscard]] bool has_own_background(const Theme& theme);

/// "Fondo: Del tema / Transparente". En config.json: "theme" / "terminal".
enum class BackgroundMode { FromTheme, Transparent };

[[nodiscard]] std::string_view background_id(BackgroundMode mode);
[[nodiscard]] std::optional<BackgroundMode> parse_background(std::string_view id);

/// Tema y modo de fondo elegidos, ya resueltos a partir de config.json.
struct Appearance {
    const Theme* theme = &default_theme();
    BackgroundMode background = BackgroundMode::FromTheme;
};

/// NO_COLOR definida y no vacía desactiva la elección de colores.
[[nodiscard]] bool no_color_enabled();

/// Resuelve lo leído de config.json ("appearance"). Un id de tema o un modo
/// de fondo desconocido usan el valor por defecto, sin fallar, y dejan un
/// aviso en warning; vacío cuenta como no definido (sin aviso).
[[nodiscard]] Appearance resolve_appearance(std::string_view theme_id,
                                            std::string_view background_id,
                                            std::string* warning = nullptr);

/// Razón de contraste WCAG 2.x entre dos colores 0xRRGGBB (de 1 a 21).
[[nodiscard]] double contrast_ratio(std::uint32_t a, std::uint32_t b);
/// Luminancia relativa WCAG 2.x de un color 0xRRGGBB (de 0 a 1).
[[nodiscard]] double relative_luminance(std::uint32_t color);

/// El color de FTXUI (Color::Default si no tiene).
[[nodiscard]] ftxui::Color to_ftxui(const ThemeColor& color);

/// Tema aplicado con su modo de fondo: lo que usan las vistas para dibujar.
/// Con fondo transparente, background y text quedan sin color; los acentos
/// conservan los del tema.
class Palette {
public:
    explicit Palette(const Theme& theme = default_theme(),
                     BackgroundMode background = BackgroundMode::FromTheme);
    explicit Palette(const Appearance& appearance)
        : Palette(*appearance.theme, appearance.background) {}

    /// El tema ya resuelto (con fondo transparente, sin background ni text).
    [[nodiscard]] const Theme& theme() const { return theme_; }
    [[nodiscard]] BackgroundMode background() const { return background_; }
    /// true si se pinta el fondo del tema en toda la pantalla.
    [[nodiscard]] bool paints_background() const;

    /// Color (y dim) de un uso; si el campo no tiene, no cambia nada.
    [[nodiscard]] ftxui::Decorator ink(ThemeColor Theme::*field) const;
    /// Fondo y texto del tema sobre todo el elemento (la raíz), o nada.
    [[nodiscard]] ftxui::Decorator base() const;
    /// Cursor de una lista, o campo o botón con el foco: selection_fg sobre
    /// selection_bg en todas las celdas del elemento, por encima de los
    /// colores y el dim de lo de adentro (o inverted, según el tema).
    [[nodiscard]] ftxui::Decorator selection() const;
    /// ==resaltado==: highlight_fg sobre highlight_bg.
    [[nodiscard]] ftxui::Decorator highlight() const;
    /// Para lo de adentro de un borde (border, window) que se colorea con
    /// ink(&Theme::border): vuelve al color de texto antes de dibujar, para
    /// que no herede el del borde. Nada si border no tiene color.
    [[nodiscard]] ftxui::Decorator inside_border() const;
    /// Como ftxui::vscroll_indicator, con la barra en el color scrollbar.
    [[nodiscard]] ftxui::Element vscroll(ftxui::Element element) const;

    /// Id del tema y modo de fondo, para las cachés de lo ya dibujado.
    [[nodiscard]] std::string key() const;

private:
    Theme theme_;
    BackgroundMode background_;
};

/// Paleta del tema "De la terminal" (los colores de antes de los temas).
[[nodiscard]] const Palette& terminal_palette();

} // namespace chatbot::cli

#endif // CHATBOT_CLI_THEME_H
