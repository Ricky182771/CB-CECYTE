#include "search_settings.h"

#include "chatbot/config.h"

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>

using chatbot::cli::SearchSettings;

TEST_CASE("SearchSettings: sin key guardada ni entorno", "[busqueda][ajustes]") {
    SearchSettings settings{"", std::nullopt};
    CHECK_FALSE(settings.key_locked());
    CHECK(settings.key_status() == "sin configurar");
    CHECK_FALSE(settings.dirty());
    CHECK_FALSE(settings.credential_update().has_value());
    CHECK_FALSE(settings.validate().has_value());
}

TEST_CASE("SearchSettings: la key escrita se guarda en \"search:tavily\"", "[busqueda][ajustes]") {
    SearchSettings settings{"", std::nullopt};
    const std::string key = "tvly-clave-ficticia-de-prueba-WXYZ";
    REQUIRE(settings.set_key(key));
    CHECK(settings.dirty());
    CHECK(settings.key_status().empty());
    CHECK_FALSE(settings.validate().has_value());
    const auto update = settings.credential_update();
    REQUIRE(update.has_value());
    CHECK(update->first == "search:tavily");
    CHECK(update->first == chatbot::kSearchCredentialsKey);
    CHECK(update->second == key);
    settings.mark_saved();
    CHECK_FALSE(settings.dirty());
    CHECK(settings.key().empty());
    CHECK(settings.key_status() == "guardada: …WXYZ");
}

TEST_CASE("SearchSettings: el entorno bloquea el campo y se muestra enmascarado",
          "[busqueda][ajustes]") {
    SearchSettings settings{"tvly-guardada-0000000000000000",
                            std::string{"tvly-del-entorno-0000000000009876"}};
    CHECK(settings.key_locked());
    CHECK_FALSE(settings.set_key("tvly-otra-clave-ficticia-000000"));
    CHECK(settings.key().empty());
    CHECK(settings.key_status() == "(definido por CHAT_SEARCH_API_KEY) …9876");
    CHECK_FALSE(settings.credential_update().has_value());
}

TEST_CASE("SearchSettings: keys incompletas se rechazan sin mostrarlas", "[busqueda][ajustes]") {
    for (const std::string key : {"$TAVILY_API_KEY", "tvly-corta", "tvly-con espacio-0000000000"}) {
        SearchSettings settings{"", std::nullopt};
        REQUIRE(settings.set_key(key));
        const std::optional<std::string> error = settings.validate();
        REQUIRE(error.has_value());
        CHECK(error->find(key) == std::string::npos);
        CHECK_FALSE(settings.credential_update().has_value());
    }
}

TEST_CASE("SearchSettings: la máscara nunca muestra más de 4 caracteres",
          "[busqueda][ajustes]") {
    const SearchSettings settings{"tvly-guardada-secreta-0123456789abcd", std::nullopt};
    const std::string status = settings.key_status();
    CHECK(status == "guardada: …abcd");
    CHECK(status.find("secreta") == std::string::npos);
}
