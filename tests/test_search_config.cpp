#include <catch2/catch_test_macros.hpp>

#include "chatbot/config.h"
#include "fake_transport.hpp"

#include <sys/stat.h>
#include <filesystem>
#include <fstream>
#include <string>

using namespace chatbot;
using namespace chatbot_test;

namespace {

/// Crea un directorio temporal para las pruebas.
std::filesystem::path make_temp_dir() {
    std::filesystem::path temp = std::filesystem::temp_directory_path();
    temp /= "chatbot_search_config_test_XXXXXX";
    std::string temp_str = temp.string();
    if (::mkdtemp(temp_str.data()) == nullptr) {
        throw std::runtime_error("mkdtemp falló");
    }
    return std::filesystem::path{temp_str};
}

/// Escribe un archivo.
void write_file(const std::filesystem::path& path, std::string_view content) {
    std::ofstream file{path};
    file << content;
    if (!file) {
        throw std::runtime_error("No se pudo escribir " + path.string());
    }
}

/// Cambia los permisos de un archivo.
void set_permissions(const std::filesystem::path& path, mode_t mode) {
    if (::chmod(path.string().c_str(), mode) != 0) {
        throw std::runtime_error("chmod falló en " + path.string());
    }
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
