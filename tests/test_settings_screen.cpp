#include "settings_screen.h"
#include "system_prompt.h"

#include "blocking_transport.hpp"
#include "fake_transport.hpp"
#include "no_color_guard.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <ftxui/component/component_base.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>

#include <optional>
#include <string>
#include <utility>

namespace {

struct Position {
    int x = -1;
    int y = -1;
};

Position find(const ftxui::Screen& screen, const std::string& needle) {
    for (int y = 0; y < screen.dimy(); ++y) {
        for (int x = 0; x < screen.dimx(); ++x) {
            std::string text;
            for (int k = x; k < screen.dimx() && text.size() < needle.size(); ++k) {
                const auto& character = screen.CellAt(k, y).character;
                text += character.empty() ? " " : character;
            }
            if (text == needle) {
                return {x, y};
            }
        }
    }
    return {};
}

struct Harness {
    chatbot_test::TaskQueue queue;
    chatbot::cli::Palette palette{chatbot::cli::resolve_appearance("", "")};
    std::optional<std::string> saved_model;
    int appearance_saves = 0;
    int prompt_saves = 0;
    std::optional<std::string> saved_prompt; ///< Lo que recibió el último guardado.
    std::optional<std::string> prompt_error; ///< Error que devuelve el guardado.
    int search_key_saves = 0;
    std::string saved_search_key; ///< Lo que recibió el último guardado de la key de búsqueda.
    int closes = 0;
    std::string base_url = "http://localhost:8080/v1"; ///< La de open(), en "custom".
    chatbot::cli::ModelsLoader loader{
        [] {
            auto transport = std::make_unique<chatbot_test::FakeTransport>();
            chatbot::HttpResponse response;
            response.status = 200;
            response.body = "{\"data\":[";
            for (int i = 0; i < 50; ++i) {
                if (i != 0) {
                    response.body += ',';
                }
                response.body += "{\"id\":\"modelo-" + std::to_string(i) + "\"}";
            }
            response.body += "]}";
            transport->responses.push_back(std::move(response));
            return transport;
        },
        [this](auto task) { queue.post(std::move(task)); }};
    chatbot::cli::SettingsScreen settings{
        loader, palette,
        [this](const chatbot::cli::ProviderSettings& provider) -> std::optional<std::string> {
            saved_model = provider.model();
            return std::nullopt;
        },
        [this](const chatbot::cli::Appearance&) -> std::optional<std::string> {
            ++appearance_saves;
            return std::nullopt;
        },
        [this](const std::optional<std::string>& prompt) -> std::optional<std::string> {
            ++prompt_saves;
            saved_prompt = prompt;
            return prompt_error;
        },
        [this](const std::string& key) -> std::optional<std::string> {
            ++search_key_saves;
            saved_search_key = key;
            return std::nullopt;
        },
        [this] { ++closes; }};

    void open(std::string model, std::string key = {},
              std::string system_prompt = std::string{chatbot::cli::kDefaultSystemPrompt},
              std::size_t history_limit = 32000,
              chatbot::cli::SearchSettings search = {}) {
        chatbot::Config config;
        config.base_url = base_url;
        config.model = model;
        config.history_limit_bytes = history_limit;
        chatbot::cli::ProviderSettings provider({"custom", config.base_url, std::move(model)},
                                                 {}, {});
        (void)provider.set_key(std::move(key));
        settings.open(std::move(provider), config,
                      chatbot::cli::resolve_appearance("catppuccin-mocha", "theme"),
                      std::move(system_prompt), std::move(search));
        REQUIRE(queue.run_until([this] { return !loader.busy(); }));
    }

    ftxui::Screen draw(int width = 120) {
        auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(width),
                                            ftxui::Dimension::Fixed(24));
        ftxui::Render(screen, settings.component()->Render());
        return screen;
    }

    bool mouse(Position position, ftxui::Mouse::Button button,
               ftxui::Mouse::Motion motion = ftxui::Mouse::Pressed) {
        ftxui::Mouse mouse;
        mouse.x = position.x;
        mouse.y = position.y;
        mouse.button = button;
        mouse.motion = motion;
        return settings.component()->OnEvent(ftxui::Event::Mouse("", mouse));
    }

    void save() {
        const auto screen = draw();
        const Position button = find(screen, "[ Guardar ]");
        REQUIRE(button.x >= 0);
        REQUIRE(mouse(button, ftxui::Mouse::Left));
    }

    bool key(const ftxui::Event& event) { return settings.component()->OnEvent(event); }

    /// Elige "Instrucciones del sistema" y pasa al campo de texto con Tab.
    void go_to_prompt() {
        REQUIRE(key(ftxui::Event::ArrowDown));
        REQUIRE(key(ftxui::Event::ArrowDown));
        REQUIRE(find(draw(), "● Instrucciones del sistema").x >= 0);
        REQUIRE(key(ftxui::Event::Tab));
    }

    /// Elige "Búsqueda web" (sin pasar al campo).
    void go_to_search() {
        for (int i = 0; i < 3; ++i) {
            REQUIRE(key(ftxui::Event::ArrowDown));
        }
        REQUIRE(find(draw(), "● Búsqueda web").x >= 0);
    }
};

} // namespace

TEST_CASE("configuración: clic elige el modelo con y sin desplazamiento", "[ajustes][raton]") {
    const chatbot_test::NoColorGuard environment(nullptr);
    const int initial = GENERATE(0, 35);
    Harness h;
    h.open("modelo-" + std::to_string(initial));
    auto screen = h.draw();
    const std::string target = "modelo-" + std::to_string(initial + 1);
    const Position row = find(screen, target);
    REQUIRE(row.x >= 0);
    REQUIRE(h.mouse(row, ftxui::Mouse::Left));
    screen = h.draw();
    CHECK(find(screen, "● " + target).x >= 0);
    h.save();
    REQUIRE(h.saved_model.has_value());
    CHECK(*h.saved_model == target);
}

TEST_CASE("configuración: rueda solo mueve el cursor dentro de la lista", "[ajustes][raton]") {
    const chatbot_test::NoColorGuard environment(nullptr);
    Harness h;
    h.open("modelo-35");
    auto screen = h.draw();
    Position row = find(screen, "modelo-35");
    REQUIRE(row.x >= 0);
    REQUIRE(h.mouse(row, ftxui::Mouse::WheelDown));
    screen = h.draw();
    row = find(screen, "modelo-36");
    REQUIRE(row.x >= 0);
    const auto selection = screen.CellAt(row.x, row.y).background_color;
    CHECK(selection == chatbot::cli::to_ftxui(h.palette.theme().selection_bg));
    (void)h.mouse({119, 23}, ftxui::Mouse::WheelDown);
    screen = h.draw();
    row = find(screen, "modelo-36");
    REQUIRE(row.x >= 0);
    CHECK(screen.CellAt(row.x, row.y).background_color == selection);
    REQUIRE(h.settings.component()->OnEvent(ftxui::Event::Return));
    h.save();
    REQUIRE(h.saved_model.has_value());
    CHECK(*h.saved_model == "modelo-36");
}

TEST_CASE("configuración: aviso de URL sin ruta bajo el campo, sin mover los campos",
          "[ajustes][url]") {
    for (const int width : {120, 100, 60, 40}) {
        INFO("ancho " << width);
        Harness with_path;
        with_path.open("modelo-1");
        const auto reference = with_path.draw(width);
        CHECK(find(reference, "Casi todos").x < 0);

        Harness root;
        root.base_url = "http://localhost:8080";
        root.open("modelo-1");
        const auto screen = root.draw(width);
        const Position url = find(screen, "URL base");
        REQUIRE(url.x >= 0);
        if (width >= 60) {
            const Position shown = find(screen, "Casi todos");
            REQUIRE(shown.x >= 0);
            CHECK(shown.y == url.y + 1);
            CHECK(shown.x == url.x + 12); // Alineado con el campo.
        }
        // Las filas están también sin aviso: los campos quedan donde estaban.
        for (const char* field : {"URL base", "API key", "Modelos", "[ Guardar ]"}) {
            INFO(field);
            if (width >= 60) {
                CHECK(find(reference, field).x >= 0);
            }
            CHECK(find(screen, field).y == find(reference, field).y);
            CHECK(find(screen, field).x == find(reference, field).x);
        }
        // Completo en dos filas desde 100 columnas; más angosta, se recorta.
        if (width >= 100) {
            CHECK(find(screen, "https://servidor/v1).").y == url.y + 2);
        }
    }
}

TEST_CASE("configuración: el aviso de URL usa la tinta de los avisos, también con NO_COLOR",
          "[ajustes][url][tema]") {
    const char* no_color = GENERATE(static_cast<const char*>(nullptr), "1");
    const chatbot_test::NoColorGuard environment(no_color);
    Harness h;
    h.base_url = "http://localhost:8080";
    h.open("modelo-1");
    const auto screen = h.draw();
    const Position hint = find(screen, "Casi todos");
    const Position other = find(screen, "opcional en local"); // Otro aviso (key_status).
    REQUIRE(hint.x >= 0);
    REQUIRE(other.x >= 0);
    const auto& hint_cell = screen.CellAt(hint.x, hint.y);
    const auto& other_cell = screen.CellAt(other.x, other.y);
    CHECK(hint_cell.foreground_color == other_cell.foreground_color);
    CHECK(hint_cell.dim == other_cell.dim);
    if (no_color != nullptr) {
        CHECK(hint_cell.dim);
    }
}

TEST_CASE("configuración: NO_COLOR bloquea tema y fondo y no los guarda", "[ajustes][tema]") {
    const chatbot_test::NoColorGuard environment("1");
    Harness h;
    h.open("modelo-0");
    REQUIRE(h.settings.component()->OnEvent(ftxui::Event::ArrowDown));
    auto screen = h.draw();
    CHECK(find(screen, "Desactivado por NO_COLOR").x >= 0);
    CHECK(find(screen, "De la terminal").x >= 0);
    CHECK(find(screen, "Transparente").x >= 0);
    REQUIRE(h.settings.component()->OnEvent(ftxui::Event::Tab));
    screen = h.draw();
    const Position save = find(screen, "[ Guardar ]");
    REQUIRE(save.x >= 0);
    CHECK(screen.CellAt(save.x, save.y).inverted);
    REQUIRE(h.settings.component()->OnEvent(ftxui::Event::Return));
    CHECK(h.appearance_saves == 0);
    CHECK_FALSE(h.settings.is_open());
}

TEST_CASE("configuración: Guardar rechaza keys incompletas sin llamar al guardado", "[ajustes][key]") {
    const chatbot_test::NoColorGuard environment(nullptr);
    const std::string key = GENERATE(std::string{"$NIMKEY"}, std::string{"  "},
                                      std::string{"abc"}, std::string(30, 'a') + "\n");
    Harness h;
    h.open("modelo-0", key);
    h.save();
    CHECK_FALSE(h.saved_model.has_value());
    CHECK(h.settings.is_open());
    const auto screen = h.draw();
    CHECK(find(screen, "Eso parece el nombre de una variable").x >= 0);
    CHECK(find(screen, "pega la key completa.").x >= 0);
    if (key == "$NIMKEY" || key == "abc") {
        CHECK(find(screen, key).x < 0);
    }
}

TEST_CASE("configuración: Guardar acepta una key sintética completa", "[ajustes][key]") {
    const chatbot_test::NoColorGuard environment(nullptr);
    Harness h;
    h.open("modelo-0", std::string(70, 'x'));
    h.save();
    CHECK(h.saved_model == "modelo-0");
    CHECK_FALSE(h.settings.is_open());
}

TEST_CASE("configuración: escribir instrucciones y guardarlas", "[ajustes][sistema]") {
    const chatbot_test::NoColorGuard environment(nullptr);
    Harness h;
    h.open("modelo-0", {}, "");
    h.go_to_prompt();
    auto screen = h.draw();
    CHECK(find(screen, "0 / 8000 bytes").x >= 0);
    CHECK(find(screen, "Vacío: sin instrucciones de sistema").x >= 0);
    CHECK(find(screen, "[ Restaurar predeterminado ]").x >= 0);

    REQUIRE(h.key(ftxui::Event::Character("Responde solo")));
    REQUIRE(h.key(ftxui::Event::Return)); // Enter: salto de línea, no guarda.
    REQUIRE(h.key(ftxui::Event::Character("con PIÑA")));
    CHECK(h.prompt_saves == 0);
    CHECK(h.settings.is_open());
    screen = h.draw();
    const Position first = find(screen, "Responde solo");
    const Position second = find(screen, "con PIÑA");
    REQUIRE(first.x >= 0);
    REQUIRE(second.x >= 0);
    CHECK(second.y == first.y + 1);
    // "Responde solo\ncon PIÑA": 13 + 1 + 9 bytes (la Ñ son dos).
    CHECK(find(screen, "23 / 8000 bytes").x >= 0);

    h.save();
    CHECK(h.prompt_saves == 1);
    CHECK(h.saved_prompt == std::string{"Responde solo\ncon PIÑA"});
    CHECK_FALSE(h.saved_model.has_value()); // Solo cambiaron las instrucciones.
    CHECK(h.appearance_saves == 0);
    CHECK_FALSE(h.settings.is_open());
}

TEST_CASE("configuración: vaciar las instrucciones guarda la cadena vacía",
          "[ajustes][sistema]") {
    const chatbot_test::NoColorGuard environment(nullptr);
    Harness h;
    h.open("modelo-0", {}, "abc");
    h.go_to_prompt();
    REQUIRE(h.key(ftxui::Event::End));
    for (int i = 0; i < 3; ++i) {
        REQUIRE(h.key(ftxui::Event::Backspace));
    }
    h.save();
    CHECK(h.prompt_saves == 1);
    CHECK(h.saved_prompt == std::string{});
    CHECK_FALSE(h.settings.is_open());
}

TEST_CASE("configuración: Restaurar predeterminado borra la llave al guardar",
          "[ajustes][sistema]") {
    const chatbot_test::NoColorGuard environment(nullptr);
    Harness h;
    h.open("modelo-0", {}, "Mis instrucciones");
    h.go_to_prompt();
    CHECK(find(h.draw(), "Mis instrucciones").x >= 0);
    REQUIRE(h.key(ftxui::Event::Tab)); // Al botón.
    REQUIRE(h.key(ftxui::Event::Return));
    const auto screen = h.draw();
    CHECK(find(screen, "Mis instrucciones").x < 0);
    CHECK(find(screen, "Eres un asistente útil.").x >= 0);
    h.save();
    CHECK(h.prompt_saves == 1);
    CHECK_FALSE(h.saved_prompt.has_value()); // nullopt: se borra la llave.
    CHECK_FALSE(h.settings.is_open());
}

TEST_CASE("configuración: sin cambios en las instrucciones no se guardan",
          "[ajustes][sistema]") {
    const chatbot_test::NoColorGuard environment(nullptr);
    Harness h;
    h.open("modelo-0");
    h.go_to_prompt();
    REQUIRE(h.key(ftxui::Event::Tab));
    REQUIRE(h.key(ftxui::Event::Return)); // Restaurar con el predeterminado ya puesto.
    h.save();
    CHECK(h.prompt_saves == 0);
    CHECK_FALSE(h.settings.is_open());
}

TEST_CASE("configuración: descartar cambios de las instrucciones", "[ajustes][sistema]") {
    const chatbot_test::NoColorGuard environment(nullptr);
    Harness h;
    h.open("modelo-0", {}, "Original");
    h.go_to_prompt();
    REQUIRE(h.key(ftxui::Event::Character("x")));
    REQUIRE(h.key(ftxui::Event::Escape));
    CHECK(find(h.draw(), "¿Descartar los cambios? (s/n)").x >= 0);
    REQUIRE(h.key(ftxui::Event::Character("n")));
    CHECK(h.settings.is_open());
    CHECK(find(h.draw(), "¿Descartar los cambios? (s/n)").x < 0);
    CHECK(find(h.draw(), "xOriginal").x >= 0); // El cambio sigue ahí.

    REQUIRE(h.key(ftxui::Event::Escape));
    REQUIRE(h.key(ftxui::Event::Character("s")));
    CHECK_FALSE(h.settings.is_open());
    CHECK(h.prompt_saves == 0);
    CHECK(h.closes == 1);

    // Al abrir de nuevo, el campo tiene las vigentes.
    h.open("modelo-0", {}, "Original");
    h.go_to_prompt();
    const auto screen = h.draw();
    CHECK(find(screen, "Original").x >= 0);
    CHECK(find(screen, "xOriginal").x < 0);
}

TEST_CASE("configuración: Tab y Shift+Tab recorren las instrucciones", "[ajustes][sistema]") {
    const chatbot_test::NoColorGuard environment(nullptr);
    Harness h;
    h.open("modelo-0", {}, "");
    h.go_to_prompt();
    REQUIRE(h.key(ftxui::Event::Character("a"))); // El campo tiene el foco.
    REQUIRE(h.key(ftxui::Event::Tab));             // Restaurar.
    REQUIRE(h.key(ftxui::Event::Tab));             // Guardar.
    auto screen = h.draw();
    Position save = find(screen, "[ Guardar ]");
    REQUIRE(save.x >= 0);
    CHECK(screen.CellAt(save.x, save.y).background_color ==
          chatbot::cli::to_ftxui(h.palette.theme().selection_bg));
    REQUIRE(h.key(ftxui::Event::TabReverse)); // Restaurar.
    REQUIRE(h.key(ftxui::Event::TabReverse)); // El campo.
    REQUIRE(h.key(ftxui::Event::Character("b")));
    CHECK(find(h.draw(), "ab").x >= 0);
}

TEST_CASE("configuración: instrucciones inválidas no se guardan y el error se ve",
          "[ajustes][sistema]") {
    const chatbot_test::NoColorGuard environment(nullptr);
    Harness h;
    SECTION("control") {
        // Así pudo quedar en config.json editado a mano.
        h.open("modelo-0", {}, "uno\x1b[31mdos");
        h.go_to_prompt();
        // El aviso aparece antes de guardar: el campo no dibuja el control.
        CHECK(find(h.draw(), "carácter de control no permitido").x >= 0);
        REQUIRE(h.key(ftxui::Event::Character("a")));
    }
    SECTION("largo") {
        h.open("modelo-0", {}, std::string(chatbot::cli::kMaxSystemPromptBytes, 'a'));
        h.go_to_prompt();
        CHECK(find(h.draw(), "8000 / 8000 bytes").x >= 0);
        REQUIRE(h.key(ftxui::Event::Character("a")));
        CHECK(find(h.draw(), "8001 / 8000 bytes").x >= 0);
        CHECK(find(h.draw(), "el máximo es 8000").x >= 0);
    }
    h.save();
    CHECK(h.prompt_saves == 0);
    CHECK(h.settings.is_open());
    CHECK_FALSE(h.saved_model.has_value()); // Nada se guarda a medias.
    const auto screen = h.draw();
    CHECK((find(screen, "carácter de control no permitido").x >= 0 ||
           find(screen, "el máximo es 8000").x >= 0));
}

TEST_CASE("configuración: aviso si las instrucciones llenan el límite del historial",
          "[ajustes][sistema]") {
    const chatbot_test::NoColorGuard environment(nullptr);
    Harness h;
    h.open("modelo-0", {}, "", 10);
    h.go_to_prompt();
    CHECK(find(h.draw(), "límite del historial").x < 0);
    REQUIRE(h.key(ftxui::Event::Character("0123456789")));
    CHECK(find(h.draw(), "límite del historial").x >= 0);
    h.save(); // Se guarda igual.
    CHECK(h.prompt_saves == 1);
    CHECK(h.saved_prompt == std::string{"0123456789"});
}

TEST_CASE("configuración: el error del guardado de instrucciones se muestra",
          "[ajustes][sistema]") {
    const chatbot_test::NoColorGuard environment(nullptr);
    Harness h;
    h.prompt_error = "No se guardó la configuración: disco lleno";
    h.open("modelo-0", {}, "");
    h.go_to_prompt();
    REQUIRE(h.key(ftxui::Event::Character("hola")));
    h.save();
    CHECK(h.prompt_saves == 1);
    CHECK(h.settings.is_open());
    CHECK(find(h.draw(), "disco lleno").x >= 0);
}

TEST_CASE("configuración: guardar la key de búsqueda", "[ajustes][busqueda]") {
    const chatbot_test::NoColorGuard environment(nullptr);
    Harness h;
    h.open("modelo-0", {}, std::string{chatbot::cli::kDefaultSystemPrompt}, 32000,
           chatbot::cli::SearchSettings{"", std::nullopt});
    h.go_to_search();
    auto screen = h.draw();
    CHECK(find(screen, "Búsqueda web").x >= 0);
    CHECK(find(screen, "sin configurar").x >= 0);
    CHECK(find(screen, "Tavily: 1,000 búsquedas gratis al mes.").x >= 0);
    CHECK(find(screen, "nunca la conversación.").x >= 0);

    REQUIRE(h.key(ftxui::Event::Tab)); // El campo de la key.
    const std::string key = "tvly-clave-ficticia-de-prueba-WXYZ";
    REQUIRE(h.key(ftxui::Event::Character(key)));
    screen = h.draw();
    CHECK(find(screen, "tvly").x < 0); // Nunca en claro.

    h.save();
    CHECK(h.search_key_saves == 1);
    CHECK(h.saved_search_key == key);
    CHECK_FALSE(h.saved_model.has_value()); // Solo cambió la key de búsqueda.
    CHECK(h.prompt_saves == 0);
    CHECK(h.appearance_saves == 0);
    CHECK_FALSE(h.settings.is_open());
}

TEST_CASE("configuración: key de búsqueda guardada se ve enmascarada", "[ajustes][busqueda]") {
    const chatbot_test::NoColorGuard environment(nullptr);
    Harness h;
    h.open("modelo-0", {}, std::string{chatbot::cli::kDefaultSystemPrompt}, 32000,
           chatbot::cli::SearchSettings{"tvly-guardada-secreta-0123456789abcd", std::nullopt});
    h.go_to_search();
    const auto screen = h.draw();
    CHECK(find(screen, "guardada: …abcd").x >= 0);
    CHECK(find(screen, "secreta").x < 0);
    CHECK(find(screen, "0123456789").x < 0);
}

TEST_CASE("configuración: CHAT_SEARCH_API_KEY bloquea el campo de búsqueda",
          "[ajustes][busqueda]") {
    const chatbot_test::NoColorGuard environment(nullptr);
    Harness h;
    h.open("modelo-0", {}, std::string{chatbot::cli::kDefaultSystemPrompt}, 32000,
           chatbot::cli::SearchSettings{"tvly-guardada-0000000000000000",
                                        std::string{"tvly-del-entorno-0000000000009876"}});
    h.go_to_search();
    auto screen = h.draw();
    CHECK(find(screen, "(definido por CHAT_SEARCH_API_KEY) …9876").x >= 0);
    CHECK(find(screen, "escribe la API key de Tavily").x < 0); // Sin campo.
    // Tab pasa de las categorías directo a Guardar.
    REQUIRE(h.key(ftxui::Event::Tab));
    screen = h.draw();
    const Position save = find(screen, "[ Guardar ]");
    REQUIRE(save.x >= 0);
    CHECK(screen.CellAt(save.x, save.y).background_color ==
          chatbot::cli::to_ftxui(h.palette.theme().selection_bg));
    // Escribir no cambia nada: no hay cambios que guardar ni que descartar.
    (void)h.key(ftxui::Event::Character("x"));
    REQUIRE(h.key(ftxui::Event::Escape));
    CHECK_FALSE(h.settings.is_open());
    CHECK(h.search_key_saves == 0);
}

TEST_CASE("configuración: key de búsqueda incompleta no se guarda", "[ajustes][busqueda]") {
    const chatbot_test::NoColorGuard environment(nullptr);
    Harness h;
    h.open("modelo-0", {}, std::string{chatbot::cli::kDefaultSystemPrompt}, 32000,
           chatbot::cli::SearchSettings{"", std::nullopt});
    h.go_to_search();
    REQUIRE(h.key(ftxui::Event::Tab));
    REQUIRE(h.key(ftxui::Event::Character("$TAVILY_KEY")));
    h.save();
    CHECK(h.search_key_saves == 0);
    CHECK_FALSE(h.saved_model.has_value());
    CHECK(h.settings.is_open());
    const auto screen = h.draw();
    CHECK(find(screen, "key de búsqueda incompleta").x >= 0);
    CHECK(find(screen, "$TAVILY_KEY").x < 0);
}

TEST_CASE("configuración: descartar una key de búsqueda escrita pregunta",
          "[ajustes][busqueda]") {
    const chatbot_test::NoColorGuard environment(nullptr);
    Harness h;
    h.open("modelo-0", {}, std::string{chatbot::cli::kDefaultSystemPrompt}, 32000,
           chatbot::cli::SearchSettings{"", std::nullopt});
    h.go_to_search();
    REQUIRE(h.key(ftxui::Event::Tab));
    REQUIRE(h.key(ftxui::Event::Character("tvly-clave-ficticia-de-prueba-0000")));
    REQUIRE(h.key(ftxui::Event::Escape));
    CHECK(h.settings.is_open());
    CHECK(find(h.draw(), "¿Descartar los cambios? (s/n)").x >= 0);
    REQUIRE(h.key(ftxui::Event::Character("s")));
    CHECK_FALSE(h.settings.is_open());
    CHECK(h.search_key_saves == 0);
}

TEST_CASE("configuración: a dónde va un pegado según el foco", "[ajustes][pegar]") {
    const chatbot_test::NoColorGuard environment(nullptr);
    using chatbot::cli::PasteTarget;
    Harness h;
    h.open("modelo-0", {}, "");
    // Al abrir, el foco está en las categorías: el pegado se ignora.
    CHECK(h.settings.paste_target() == PasteTarget::None);

    h.go_to_prompt();
    REQUIRE(h.settings.paste_target() == PasteTarget::MultiLine);
    // Como lo hace main.cpp: un solo Event::Character con todo el texto.
    const auto paste = chatbot::cli::prepare_paste("Primera\r\n\tSegunda\n", PasteTarget::MultiLine);
    REQUIRE(paste.has_value());
    REQUIRE(h.key(ftxui::Event::Character(paste->text)));
    auto screen = h.draw();
    const Position first = find(screen, "Primera");
    const Position second = find(screen, "    Segunda");
    REQUIRE(first.x >= 0);
    REQUIRE(second.y == first.y + 1);

    REQUIRE(h.key(ftxui::Event::Tab)); // Restaurar: un botón.
    CHECK(h.settings.paste_target() == PasteTarget::None);
    REQUIRE(h.key(ftxui::Event::TabReverse));
    CHECK(h.settings.paste_target() == PasteTarget::MultiLine);

    // Con la pregunta de descartar, un pegado no la contesta.
    REQUIRE(h.key(ftxui::Event::Escape));
    REQUIRE(find(h.draw(), "¿Descartar los cambios? (s/n)").x >= 0);
    CHECK(h.settings.paste_target() == PasteTarget::None);
}

TEST_CASE("configuración: pegar una key con salto de línea, sin mostrarla", "[ajustes][pegar]") {
    const chatbot_test::NoColorGuard environment(nullptr);
    using chatbot::cli::PasteTarget;
    Harness h;
    h.open("modelo-0", {}, std::string{chatbot::cli::kDefaultSystemPrompt}, 32000,
           chatbot::cli::SearchSettings{"", std::nullopt});
    h.go_to_search();
    REQUIRE(h.key(ftxui::Event::Tab)); // El campo de la key.
    REQUIRE(h.settings.paste_target() == PasteTarget::SingleLine);
    const std::string key = "tvly-clave-ficticia-de-prueba-WXYZ";
    const auto paste = chatbot::cli::prepare_paste(key + "\r\n", PasteTarget::SingleLine);
    REQUIRE(paste.has_value());
    REQUIRE(h.key(ftxui::Event::Character(paste->text)));
    // El aviso de un pegado recortado no lleva el texto.
    h.settings.show_notice(std::string{chatbot::cli::kPasteTruncated});
    const auto screen = h.draw();
    CHECK(find(screen, "tvly").x < 0);
    CHECK(find(screen, "WXYZ").x < 0);
    CHECK(find(screen, "se pegaron los primeros 256 KiB.").x >= 0);

    h.save();
    CHECK(h.search_key_saves == 1);
    CHECK(h.saved_search_key == key);
}

TEST_CASE("configuración: pegar solo un salto de línea en las instrucciones", "[ajustes][pegar]") {
    const chatbot_test::NoColorGuard environment(nullptr);
    using chatbot::cli::PasteTarget;
    Harness h;
    h.open("modelo-0", {}, "ab");
    h.go_to_prompt();
    REQUIRE(h.settings.paste_target() == PasteTarget::MultiLine);
    REQUIRE(h.key(ftxui::Event::End));
    REQUIRE(h.key(ftxui::Event::ArrowLeft)); // El cursor entre "a" y "b".
    const auto paste = chatbot::cli::prepare_paste("\r\n", PasteTarget::MultiLine);
    REQUIRE(paste.has_value());
    REQUIRE(paste->text == "\n");
    // Como lo hace main.cpp. Event compara solo el texto: para el campo es
    // Enter, que con multiline inserta "\n" y llama a on_enter (vacío).
    REQUIRE(h.key(ftxui::Event::Character(paste->text)));
    const auto screen = h.draw();
    CHECK(find(screen, "3 / 8000 bytes").x >= 0); // "a\nb".
    // Nada más: no guarda, no cierra, no mueve el foco ni pregunta.
    CHECK(h.prompt_saves == 0);
    CHECK(h.closes == 0);
    CHECK(h.settings.is_open());
    CHECK(h.settings.paste_target() == PasteTarget::MultiLine);
    CHECK(find(screen, "¿Descartar los cambios? (s/n)").x < 0);
    // Lo siguiente que se escribe va después del salto.
    REQUIRE(h.key(ftxui::Event::Character("x")));
    h.save();
    CHECK(h.prompt_saves == 1);
    CHECK(h.saved_prompt == std::string{"a\nxb"});
}
