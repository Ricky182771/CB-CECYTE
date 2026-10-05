#include "settings_screen.h"

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
        [] {}};

    void open(std::string model, std::string key = {}) {
        chatbot::Config config;
        config.base_url = "http://localhost:8080/v1";
        config.model = model;
        chatbot::cli::ProviderSettings provider({"custom", config.base_url, std::move(model)},
                                                 {}, {});
        (void)provider.set_key(std::move(key));
        settings.open(std::move(provider), config,
                      chatbot::cli::resolve_appearance("catppuccin-mocha", "theme"));
        REQUIRE(queue.run_until([this] { return !loader.busy(); }));
    }

    ftxui::Screen draw() {
        auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(120),
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
