#include "chatbot/config.h"
#include "config_internal.h"
#include "fake_transport.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <unistd.h>

#include <cerrno>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <utility>

namespace {

using chatbot::ConfigOptions;
using chatbot::EnvLookup;
using chatbot::PathProvider;

/// Archivo JSON temporal que se borra solo al salir del ámbito.
class TempFile {
public:
    TempFile(std::string path, std::string content)
        : path_{std::move(path)} {
        std::ofstream file{path_};
        file << content;
    }
    ~TempFile() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;

    [[nodiscard]] const std::string& path() const { return path_; }

private:
    std::string path_;
};

/// Directorio temporal único que se borra al salir del ámbito.
class TempDir {
public:
    TempDir()
        : path_{std::filesystem::temp_directory_path() /
                ("chatbot_tests_" + std::to_string(static_cast<long>(::getpid())) + "_" +
                 std::to_string(++counter()))} {
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    [[nodiscard]] std::string string() const { return path_.string(); }

private:
    static int& counter() {
        static int value = 0;
        return ++value;
    }
    std::filesystem::path path_;
};

PathProvider path_of(std::optional<std::string> path) {
    return [path = std::move(path)] { return path; };
}

} // namespace

TEST_CASE("Falta la key: error Config", "[config]") {
    chatbot_test::FakeEnv env; // Sin CHAT_API_KEY.
    env.values["CHAT_MODEL"] = "modelo-x";

    const chatbot::Result<chatbot::Config> config =
        chatbot::load_config(ConfigOptions{env, path_of(std::nullopt)});

    REQUIRE(config.is_error());
    REQUIRE(config.error().kind == chatbot::ErrorKind::Config);
    REQUIRE_THAT(config.error().message,
                 Catch::Matchers::ContainsSubstring("CHAT_API_KEY"));
}

TEST_CASE("Falta el modelo: error Config con mensaje claro", "[config]") {
    chatbot_test::FakeEnv env;
    env.values["CHAT_API_KEY"] = "clave-ficticia";

    const chatbot::Result<chatbot::Config> config =
        chatbot::load_config(ConfigOptions{env, path_of(std::nullopt)});

    REQUIRE(config.is_error());
    REQUIRE(config.error().kind == chatbot::ErrorKind::Config);
    REQUIRE_THAT(config.error().message,
                 Catch::Matchers::ContainsSubstring("CHAT_MODEL"));
}

TEST_CASE("Sin archivo de configuración: se aplican los valores por defecto", "[config]") {
    chatbot_test::FakeEnv env;
    env.values["CHAT_API_KEY"] = "clave-ficticia";
    env.values["CHAT_MODEL"] = "modelo-x";

    const chatbot::Result<chatbot::Config> config =
        chatbot::load_config(ConfigOptions{env, path_of(std::nullopt)});

    REQUIRE(config.is_ok());
    REQUIRE(config.value().base_url == "https://integrate.api.nvidia.com/v1");
    REQUIRE(config.value().timeout_seconds == std::chrono::seconds{120});
}

TEST_CASE("Archivo inexistente no es error", "[config]") {
    chatbot_test::FakeEnv env;
    env.values["CHAT_API_KEY"] = "clave-ficticia";
    env.values["CHAT_MODEL"] = "modelo-x";

    const chatbot::Result<chatbot::Config> config = chatbot::load_config(
        ConfigOptions{env, path_of(std::string{"/ruta/que/no/existe/config.json"})});

    REQUIRE(config.is_ok());
    REQUIRE(config.value().base_url == "https://integrate.api.nvidia.com/v1");
}

TEST_CASE("Archivo con JSON inválido: error Config", "[config]") {
    chatbot_test::FakeEnv env;
    env.values["CHAT_API_KEY"] = "clave-ficticia";
    env.values["CHAT_MODEL"] = "modelo-x";

    TempFile archivo{"chatbot_tests_invalido.json", "{ no es json ]"};
    const chatbot::Result<chatbot::Config> config = chatbot::load_config(
        ConfigOptions{env, path_of(archivo.path())});

    REQUIRE(config.is_error());
    REQUIRE(config.error().kind == chatbot::ErrorKind::Config);
    REQUIRE_THAT(config.error().message,
                 Catch::Matchers::ContainsSubstring("configuración"));
}

TEST_CASE("El entorno pisa los valores del archivo (precedencia)", "[config]") {
    TempFile archivo{"chatbot_tests_precedencia.json",
                     R"({"base_url": "https://archivo.invalid/v1", "model": "modelo-archivo", )"
                     R"("timeout_seconds": 15})"};

    chatbot_test::FakeEnv env;
    env.values["CHAT_API_KEY"] = "clave-ficticia";
    env.values["CHAT_MODEL"] = "modelo-entorno";
    env.values["CHAT_TIMEOUT"] = "77";
    // CHAT_BASE_URL ausente: se conserva el valor del archivo.

    const chatbot::Result<chatbot::Config> config =
        chatbot::load_config(ConfigOptions{env, path_of(archivo.path())});

    REQUIRE(config.is_ok());
    CHECK(config.value().model == "modelo-entorno");
    CHECK(config.value().timeout_seconds == std::chrono::seconds{77});
    CHECK(config.value().base_url == "https://archivo.invalid/v1");
}

TEST_CASE("CHAT_TIMEOUT inválido: error Config", "[config]") {
    chatbot_test::FakeEnv env;
    env.values["CHAT_API_KEY"] = "clave-ficticia";
    env.values["CHAT_MODEL"] = "modelo-x";
    env.values["CHAT_TIMEOUT"] = "treinta";

    const chatbot::Result<chatbot::Config> config =
        chatbot::load_config(ConfigOptions{env, path_of(std::nullopt)});

    REQUIRE(config.is_error());
    REQUIRE(config.error().kind == chatbot::ErrorKind::Config);
    REQUIRE_THAT(config.error().message,
                 Catch::Matchers::ContainsSubstring("CHAT_TIMEOUT"));
}

TEST_CASE("La key nunca sale del archivo de configuración", "[config]") {
    TempFile archivo{"chatbot_tests_con_key.json",
                     R"({"api_key": "clave-prohibida", "model": "modelo-archivo"})"};

    chatbot_test::FakeEnv env;
    env.values["CHAT_API_KEY"] = "clave-ficticia";

    const chatbot::Result<chatbot::Config> config =
        chatbot::load_config(ConfigOptions{env, path_of(archivo.path())});

    REQUIRE(config.is_ok());
    CHECK(config.value().api_key == "clave-ficticia");
    // La llave desconocida se ignora por completo.
    CHECK(config.value().model == "modelo-archivo");
}

TEST_CASE("build_config_path: XDG_CONFIG_HOME presente y ausente", "[config][xdg]") {
    using chatbot::build_config_path;

    // Presente y no vacío: manda XDG, incluso si HOME también existe.
    CHECK(build_config_path(std::string{"/xdg/custom"}, std::string{"/home/usuario"})
              .value() == "/xdg/custom/chatbot/config.json");
    // Con barra final se normaliza.
    CHECK(build_config_path(std::string{"/xdg/custom/"}, std::string{"/home/usuario"})
              .value() == "/xdg/custom/chatbot/config.json");

    // Ausente: se usa $HOME/.config/chatbot/config.json.
    CHECK(build_config_path(std::nullopt, std::string{"/home/usuario"})
              .value() == "/home/usuario/.config/chatbot/config.json");
    // Presente pero vacío: cuenta como ausente.
    CHECK(build_config_path(std::string{}, std::string{"/home/usuario"})
              .value() == "/home/usuario/.config/chatbot/config.json");

    // Sin XDG ni HOME: no hay ruta determinable.
    CHECK_FALSE(build_config_path(std::nullopt, std::nullopt).has_value());
}

TEST_CASE("default_config_path usa el entorno real del proceso", "[config][xdg]") {
    // El proceso de pruebas corre con HOME definido; basta con que la función
    // devuelva una ruta coherente dentro de él.
    const std::optional<std::string> path = chatbot::default_config_path();
    const char* xdg = ::getenv("XDG_CONFIG_HOME");
    const char* home = ::getenv("HOME");

    if (xdg != nullptr && *xdg != '\0') {
        REQUIRE(path.has_value());
        REQUIRE_THAT(*path, Catch::Matchers::ContainsSubstring("/chatbot/config.json"));
    } else if (home != nullptr && *home != '\0') {
        REQUIRE(path.has_value());
        REQUIRE_THAT(*path,
                     Catch::Matchers::ContainsSubstring(".config/chatbot/config.json"));
    } else {
        CHECK_FALSE(path.has_value());
    }
}
