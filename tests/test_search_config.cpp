#include <catch2/catch_test_macros.hpp>

#include "chatbot/config.h"
#include "chatbot/credentials.h"
#include "fake_transport.hpp"
#include "posix_permissions.hpp"
#include "temp_dir.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <stdexcept>
#include <string>

using namespace chatbot;
using namespace chatbot_test;

namespace {

/// Crea un directorio temporal para las pruebas (portable, sin mkdtemp: el
/// nombre lleva un número al azar y create_directory falla si ya existe).
std::filesystem::path make_temp_dir() {
    static std::mt19937_64 generator{std::random_device{}()};
    for (int attempt = 0; attempt < 100; ++attempt) {
        const std::filesystem::path candidate =
            std::filesystem::temp_directory_path() /
            ("chatbot_search_config_test_" + std::to_string(generator()));
        if (std::filesystem::create_directory(candidate)) {
            return candidate;
        }
    }
    throw std::runtime_error("no se pudo crear un directorio temporal");
}

/// Escribe un archivo.
void write_file(const std::filesystem::path& path, std::string_view content) {
    std::ofstream file{path};
    file << content;
    if (!file) {
        throw std::runtime_error("No se pudo escribir " + path.string());
    }
}

/// Cambia los permisos de un archivo (solo en POSIX; en Windows no hace nada).
void set_permissions(const std::filesystem::path& path, unsigned mode) {
    chatbot_test::set_mode(path, mode);
}

}  // namespace

TEST_CASE("load_search_api_key: entorno tiene precedencia", "[search_config]") {
    const auto temp_dir = make_temp_dir();
    const auto creds_path = temp_dir / "credentials.json";

    // Credentials.json con una key.
    write_file(creds_path, R"({"version": 1, "keys": {"search:tavily": "key-from-file"}})");
    set_permissions(creds_path, 0600);

    // Variable de entorno.
    FakeEnv env;
    env.values["CHAT_SEARCH_API_KEY"] = "key-from-env";

    const auto result = load_search_api_key(env, creds_path.string());
    REQUIRE(result.is_ok());
    REQUIRE(result.value() == "key-from-env");

    std::filesystem::remove_all(temp_dir);
}

TEST_CASE("load_search_api_key: archivo sin la llave devuelve vacío", "[search_config]") {
    const auto temp_dir = make_temp_dir();
    const auto creds_path = temp_dir / "credentials.json";

    // Credentials.json sin "search:tavily".
    write_file(creds_path, R"({"version": 1, "keys": {"openai": "other-key"}})");
    set_permissions(creds_path, 0600);

    FakeEnv env;

    const auto result = load_search_api_key(env, creds_path.string());
    REQUIRE(result.is_ok());
    REQUIRE(result.value().empty());

    std::filesystem::remove_all(temp_dir);
}

TEST_CASE("load_search_api_key: archivo inexistente devuelve vacío", "[search_config]") {
    FakeEnv env;

    const auto result = load_search_api_key(env, "/no/existe/credentials.json");
    REQUIRE(result.is_ok());
    REQUIRE(result.value().empty());
}

#ifndef _WIN32
TEST_CASE("load_search_api_key: permisos 0644 es un error", "[search_config]") {
    const auto temp_dir = make_temp_dir();
    const auto creds_path = temp_dir / "credentials.json";

    // Credentials.json con permisos abiertos.
    write_file(creds_path, R"({"version": 1, "keys": {"search:tavily": "key"}})");
    set_permissions(creds_path, 0644);

    FakeEnv env;

    const auto result = load_search_api_key(env, creds_path.string());
    REQUIRE(result.is_error());
    REQUIRE(result.error().kind == ErrorKind::Config);
    REQUIRE(result.error().message.find("permisos") != std::string::npos);

    std::filesystem::remove_all(temp_dir);
}
#endif

TEST_CASE("load_search_api_key: JSON inválido es un error", "[search_config]") {
    const auto temp_dir = make_temp_dir();
    const auto creds_path = temp_dir / "credentials.json";

    // JSON inválido.
    write_file(creds_path, "no es json");
    set_permissions(creds_path, 0600);

    FakeEnv env;

    const auto result = load_search_api_key(env, creds_path.string());
    REQUIRE(result.is_error());
    REQUIRE(result.error().kind == ErrorKind::Config);

    std::filesystem::remove_all(temp_dir);
}

TEST_CASE("load_search_api_key: carga del archivo si el entorno no está", "[search_config]") {
    const auto temp_dir = make_temp_dir();
    const auto creds_path = temp_dir / "credentials.json";

    write_file(creds_path, R"({"version": 1, "keys": {"search:tavily": "key-from-file"}})");
    set_permissions(creds_path, 0600);

    FakeEnv env;  // Sin CHAT_SEARCH_API_KEY.

    const auto result = load_search_api_key(env, creds_path.string());
    REQUIRE(result.is_ok());
    REQUIRE(result.value() == "key-from-file");

    std::filesystem::remove_all(temp_dir);
}

TEST_CASE("search:tavily se conserva junto a las demás keys y nunca va en config.json",
          "[search_config]") {
    const chatbot_test::ScopedTempDir temp;
    const std::string creds_path = (temp.path() / "chatbot" / "credentials.json").string();
    const std::string config_path = (temp.path() / "chatbot" / "config.json").string();

    // Primero la key de búsqueda, luego la del modelo, como lo hace main.cpp:
    // leer, cambiar una llave y volver a escribir.
    Credentials first;
    first.keys[std::string{kSearchCredentialsKey}] = "tvly-clave-ficticia-de-busqueda";
    REQUIRE_FALSE(save_credentials(creds_path, first).has_value());
    Result<Credentials> loaded = load_credentials(creds_path);
    REQUIRE(loaded.is_ok());
    Credentials second = loaded.value();
    second.keys["nvidia"] = "nvapi-clave-ficticia-del-modelo";
    REQUIRE_FALSE(save_credentials(creds_path, second).has_value());
    REQUIRE_FALSE(save_config_file(config_path, {"nvidia", "https://integrate.api.nvidia.com/v1",
                                                 "modelo-de-prueba"})
                      .has_value());

    loaded = load_credentials(creds_path);
    REQUIRE(loaded.is_ok());
    CHECK(loaded.value().keys.at("search:tavily") == "tvly-clave-ficticia-de-busqueda");
    CHECK(loaded.value().keys.at("nvidia") == "nvapi-clave-ficticia-del-modelo");

    const FakeEnv env; // Sin CHAT_SEARCH_API_KEY.
    const Result<std::string> key = load_search_api_key(env, creds_path);
    REQUIRE(key.is_ok());
    CHECK(key.value() == "tvly-clave-ficticia-de-busqueda");

    std::ifstream config_file{config_path};
    const std::string config_text{std::istreambuf_iterator<char>{config_file},
                                  std::istreambuf_iterator<char>{}};
    REQUIRE_FALSE(config_text.empty());
    CHECK(config_text.find("tvly-") == std::string::npos);
    CHECK(config_text.find("search") == std::string::npos);
    CHECK(config_text.find("nvapi-") == std::string::npos);
}
