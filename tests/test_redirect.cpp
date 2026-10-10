// Redirecciones (3xx): el transporte no las sigue; el error explica a dónde
// redirige el servidor y sugiere la URL base. También base_url_has_path.

#include "chatbot/chat_client.h"
#include "chatbot/config.h"
#include "chatbot/error.h"
#include "chatbot/utf8.h"
#include "fake_transport.hpp"
#include "redirect.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace {

using Catch::Matchers::ContainsSubstring;
using chatbot::ErrorKind;
using chatbot::redirect_display;
using chatbot::suggest_base_url;

/// Mensaje genérico de hoy para un estado sin cuerpo interpretable.
std::string generic_message(int status) {
    return "El servidor respondió con el estado HTTP " + std::to_string(status) + ".";
}

/// Respuesta 3xx con el cuerpo HTML de nginx y, si se da, la URL de Location.
chatbot::HttpResponse redirect(int status, std::optional<std::string> location) {
    chatbot::HttpResponse response;
    response.status = status;
    response.body = "<html><head><title>301 Moved Permanently</title></head></html>";
    response.redirect_url = std::move(location);
    return response;
}

/// Config con la base sin /v1, como en el caso de nginx.
chatbot::Config config_without_v1() {
    chatbot::Config config = chatbot_test::test_config();
    config.base_url = "https://h";
    return config;
}

} // namespace

TEST_CASE("redirect_display: sin query, fragmento, usuario ni controles", "[redireccion]") {
    CHECK(redirect_display("https://h/v1/models") == "https://h/v1/models");
    CHECK(redirect_display("https://h/v1/models?api_key=secreto#frag") == "https://h/v1/models");
    CHECK(redirect_display("https://h/v1/models#a?b") == "https://h/v1/models");
    CHECK(redirect_display("https://usuario:clave@h/v1/models") == "https://h/v1/models");
    CHECK(redirect_display("https://h/v1/a@b") == "https://h/v1/a@b"); // @ en la ruta se queda.
    CHECK(redirect_display("https://h/v1\x1b[31m/mo\ndels\x7f") == "https://h/v1[31m/models");
    CHECK(redirect_display("https://h/\xC2\x9B" "x") == "https://h/x"); // C1 (CSI).
    CHECK(redirect_display("https://h/\xFF/models") == "https://h/\xEF\xBF\xBD/models");
    CHECK(redirect_display("https://h/ñ") == "https://h/ñ");
}

TEST_CASE("redirect_display: recorta a 200 bytes sin partir caracteres", "[redireccion]") {
    const std::string fits = "https://h/" + std::string(200 - 10, 'a');
    CHECK(redirect_display(fits) == fits);

    for (const std::string& unit : {std::string{"a"}, std::string{"ñ"}, std::string{"€"},
                                    std::string{"😀"}}) {
        for (std::size_t offset = 0; offset < 4; ++offset) {
            const std::string url = "https://h/" + std::string(offset, 'x');
            std::string long_url = url;
            while (long_url.size() < 400) {
                long_url += unit;
            }
            INFO(unit << " desfase " << offset);
            const std::string shown = redirect_display(long_url);
            CHECK(shown.size() <= chatbot::kMaxRedirectDisplayBytes);
            CHECK(shown.size() > chatbot::kMaxRedirectDisplayBytes - 7);
            CHECK(chatbot::utf8::is_valid(shown));
            CHECK_THAT(shown, Catch::Matchers::EndsWith("…"));
            CHECK(long_url.rfind(shown.substr(0, shown.size() - 3), 0) == 0);
        }
    }
}

TEST_CASE("suggest_base_url: casos de nginx y rechazos", "[redireccion]") {
    // nginx: base sin /v1.
    CHECK(suggest_base_url("https://h/models", "https://h/v1/models", "/models") ==
          std::optional<std::string>{"https://h/v1"});
    CHECK(suggest_base_url("https://h/chat/completions", "https://h/v1/chat/completions",
                           "/chat/completions") == std::optional<std::string>{"https://h/v1"});
    // Query y fragmento no cuentan.
    CHECK(suggest_base_url("https://h/models", "https://h/v1/models?x=1#y", "/models") ==
          std::optional<std::string>{"https://h/v1"});
    // http → https del mismo host.
    CHECK(suggest_base_url("http://localhost:8080/models", "https://localhost:8080/models",
                           "/models") == std::optional<std::string>{"https://localhost:8080"});
    // http:// local se acepta.
    CHECK(suggest_base_url("http://127.0.0.1:8765/models", "http://127.0.0.1:8765/v1/models",
                           "/models") == std::optional<std::string>{"http://127.0.0.1:8765/v1"});

    // Con "/" final no termina en path.
    CHECK_FALSE(suggest_base_url("https://h/models", "https://h/v1/models/", "/models"));
    // Otro endpoint.
    CHECK_FALSE(suggest_base_url("https://h/models", "https://h/login", "/models"));
    CHECK_FALSE(suggest_base_url("https://h/models", "https://h/v1/chat/completions", "/models"));
    // http:// a un host que no es local: no pasa validate_base_url.
    CHECK_FALSE(suggest_base_url("https://h/models", "http://h/v1/models", "/models"));
    // Igual a la base actual (también con "/" de más).
    CHECK_FALSE(suggest_base_url("https://h/v1/models", "https://h/v1/models", "/models"));
    CHECK_FALSE(suggest_base_url("https://h/v1/models", "https://h/v1//models", "/models"));
    // Con usuario, controles o sin esquema.
    CHECK_FALSE(suggest_base_url("https://h/models", "https://u:p@h/v1/models", "/models"));
    CHECK_FALSE(suggest_base_url("https://h/models", "https://h/v\x1b" "1/models", "/models"));
    CHECK_FALSE(suggest_base_url("https://h/models", "/v1/models", "/models"));
    CHECK_FALSE(suggest_base_url("https://h/models", "/models", "/models"));
    // Demasiado larga para mostrarse completa.
    CHECK_FALSE(suggest_base_url("https://h/models",
                                 "https://h/" + std::string(300, 'a') + "/models", "/models"));
}

TEST_CASE("Redirección: list_models, complete y complete_stream lo explican sin reintentar",
          "[redireccion][mapeo]") {
    const std::string expected_models =
        "El servidor redirige a https://h/v1/models. Revisa la URL base: probablemente debe "
        "ser https://h/v1 (CHAT_BASE_URL o \"base_url\").";
    const std::string expected_chat =
        "El servidor redirige a https://h/v1/chat/completions. Revisa la URL base: "
        "probablemente debe ser https://h/v1 (CHAT_BASE_URL o \"base_url\").";

    for (const int status : {301, 302, 307, 308}) {
        INFO("estado " << status);
        {
            auto harness = chatbot_test::make_client(config_without_v1());
            harness.transport->responses.push_back(redirect(status, "https://h/v1/models"));
            const auto models = harness.client->list_models();
            REQUIRE(models.is_error());
            CHECK(models.error().kind == ErrorKind::BadResponse);
            CHECK(models.error().http_status == status);
            CHECK(models.error().message == expected_models);
            CHECK(harness.transport->requests.size() == 1);
            CHECK(harness.sleeper->sleeps.empty());
        }
        {
            auto harness = chatbot_test::make_client(config_without_v1());
            harness.transport->responses.push_back(
                redirect(status, "https://h/v1/chat/completions"));
            const auto reply = harness.client->complete(chatbot_test::sample_messages());
            REQUIRE(reply.is_error());
            CHECK(reply.error().kind == ErrorKind::BadResponse);
            CHECK(reply.error().message == expected_chat);
            CHECK(harness.transport->requests.size() == 1);
            CHECK(harness.sleeper->sleeps.empty());
        }
        {
            auto harness = chatbot_test::make_client(config_without_v1());
            harness.transport->responses.push_back(
                redirect(status, "https://h/v1/chat/completions"));
            bool delta = false;
            const auto reply = harness.client->complete_stream(
                chatbot_test::sample_messages(), [&delta](std::string_view) {
                    delta = true;
                    return true;
                });
            REQUIRE(reply.is_error());
            CHECK(reply.error().kind == ErrorKind::BadResponse);
            CHECK(reply.error().message == expected_chat);
            CHECK_FALSE(delta);
            CHECK(harness.transport->requests.size() == 1);
            CHECK(harness.sleeper->sleeps.empty());
        }
    }
}

TEST_CASE("Redirección sin sugerencia y sin Location", "[redireccion][mapeo]") {
    auto other = chatbot_test::make_client(config_without_v1());
    other.transport->responses.push_back(redirect(302, "https://h/login?next=%2Fmodels"));
    const auto models = other.client->list_models();
    REQUIRE(models.is_error());
    CHECK(models.error().message ==
          "El servidor redirige a https://h/login. Revisa la URL base (CHAT_BASE_URL o "
          "\"base_url\"): puede faltarle o sobrarle parte de la ruta, como /v1.");

    // La sugerencia sería la misma base, o la redirección termina en "/".
    for (const char* location :
         {"https://pruebas.invalid/v1/models", "https://pruebas.invalid/v1/models/"}) {
        INFO(location);
        auto same = chatbot_test::make_client(); // https://pruebas.invalid/v1
        same.transport->responses.push_back(redirect(301, location));
        const auto same_models = same.client->list_models();
        REQUIRE(same_models.is_error());
        CHECK_THAT(same_models.error().message,
                   ContainsSubstring(std::string{"redirige a "} + location + ".") &&
                       ContainsSubstring("puede faltarle o sobrarle"));
    }

    for (const int status : {301, 302, 307, 308}) {
        auto harness = chatbot_test::make_client(config_without_v1());
        harness.transport->responses.push_back(redirect(status, std::nullopt));
        const auto reply = harness.client->complete(chatbot_test::sample_messages());
        REQUIRE(reply.is_error());
        CHECK(reply.error().kind == ErrorKind::BadResponse);
        CHECK(reply.error().message == generic_message(status));
        CHECK(harness.transport->requests.size() == 1);
    }

    // Fuera de 3xx, redirect_url no cambia nada.
    auto not_found = chatbot_test::make_client(config_without_v1());
    not_found.transport->responses.push_back(redirect(404, "https://h/v1/models"));
    const auto missing = not_found.client->list_models();
    REQUIRE(missing.is_error());
    CHECK(missing.error().kind == ErrorKind::ModelNotFound);
    CHECK(missing.error().message == generic_message(404));
}

TEST_CASE("Redirección: el mensaje nunca contiene la key", "[redireccion][seguridad]") {
    const chatbot::Config config = config_without_v1();
    const std::string& key = config.api_key;
    REQUIRE_FALSE(key.empty());

    // En la query (como si el servidor la reflejara) y en el usuario: se quitan.
    for (const std::string& location :
         {"https://h/v1/models?api_key=" + key, "https://h/v1/models#" + key,
          "https://" + key + "@h/v1/models", "https://u:" + key + "@h/v1/models"}) {
        INFO(location);
        auto harness = chatbot_test::make_client(config);
        harness.transport->responses.push_back(redirect(301, location));
        const auto models = harness.client->list_models();
        REQUIRE(models.is_error());
        CHECK_THAT(models.error().message, !ContainsSubstring(key));
        CHECK_THAT(models.error().message, ContainsSubstring("redirige a https://h/v1/models"));
    }

    // En la ruta: no se puede quitar, así que queda el mensaje genérico.
    for (const std::string& location :
         {"https://h/" + key + "/models", "https://h/v1/models/" + key}) {
        INFO(location);
        auto harness = chatbot_test::make_client(config);
        harness.transport->responses.push_back(redirect(301, location));
        const auto models = harness.client->list_models();
        REQUIRE(models.is_error());
        CHECK(models.error().message == generic_message(301));
    }
}

TEST_CASE("base_url_has_path: ruta después del host y el puerto", "[proveedor][url]") {
    for (const char* url : {"https://h/v1", "https://h:8443/v1/", "http://localhost:11434/v1",
                            "https://h/openai/v1?x=1"}) {
        INFO(url);
        CHECK(chatbot::base_url_has_path(url));
    }
    for (const char* url : {"https://h", "https://h/", "https://h:8443", "https://h//",
                            "https://h?x=1", "https://h/#v1", "http://[::1]:8080",
                            "http://h/v1", "no es url", ""}) {
        INFO(url);
        CHECK_FALSE(chatbot::base_url_has_path(url));
    }
}
