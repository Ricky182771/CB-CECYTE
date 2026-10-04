// Núcleo de la configuración de proveedores: URL base (http:// solo local),
// key opcional en local, GET /models, credentials.json y guardado de
// config.json conservando llaves.

#include "chatbot/cancel_token.h"
#include "chatbot/chat_client.h"
#include "chatbot/config.h"
#include "chatbot/credentials.h"
#include "fake_transport.hpp"
#include "temp_dir.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <sys/stat.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

using chatbot::ErrorKind;
using chatbot_test::FakeEnv;
using chatbot_test::ScopedTempDir;

constexpr const char* kSecret = "sk-clave-secreta-de-prueba-1234";

chatbot::HttpResponse response(int status, std::string body) {
    chatbot::HttpResponse r;
    r.status = status;
    r.body = std::move(body);
    return r;
}

std::string read(const std::filesystem::path& path) {
    std::ifstream file{path};
    std::ostringstream content;
    content << file.rdbuf();
    return content.str();
}

void write(const std::filesystem::path& path, const std::string& content, mode_t mode) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream{path} << content;
    ::chmod(path.c_str(), mode);
}

mode_t mode_of(const std::filesystem::path& path) {
    struct stat info {};
    REQUIRE(::stat(path.c_str(), &info) == 0);
    return info.st_mode & 0777;
}

chatbot::Result<chatbot::Config> load(const FakeEnv& env, const std::filesystem::path& config) {
    return chatbot::load_config(chatbot::ConfigOptions{
        env, [config] { return std::optional<std::string>{config.string()}; }, {}});
}

} // namespace

TEST_CASE("URL base: https cualquiera y http solo local", "[proveedor][url]") {
    for (const char* url : {"http://localhost:11434/v1", "http://127.0.0.1:1234/v1",
                            "http://[::1]:11434/v1", "http://localhost/v1", "HTTP://LocalHost:8080",
                            "https://api.openai.com/v1", "https://192.168.1.10:8443/v1"}) {
        INFO(url);
        CHECK_FALSE(chatbot::validate_base_url(url).has_value());
    }
    for (const char* url :
         {"http://192.168.1.10/v1", "http://localhost.evil.com/v1", "http://localhost@evil.com/v1",
          "https://usuario:clave@api.openai.com/v1", "ftp://localhost/v1", "http://127.0.0.2/v1",
          "http://[::1/v1", "http://localhost:/v1", "http://localhost:99999/v1", "localhost:11434/v1",
          "https:///v1", ""}) {
        INFO(url);
        const std::optional<chatbot::ChatError> error = chatbot::validate_base_url(url);
        REQUIRE(error.has_value());
        CHECK(error->kind == ErrorKind::Config);
    }
    CHECK(chatbot::is_local_base_url("http://localhost:11434/v1"));
    CHECK(chatbot::is_local_base_url("https://127.0.0.1/v1"));
    CHECK_FALSE(chatbot::is_local_base_url("http://localhost.evil.com/v1"));
    CHECK_FALSE(chatbot::is_local_base_url("https://api.openai.com/v1"));
}

TEST_CASE("Key vacía: válida en local, error Config en remoto", "[proveedor][key]") {
    chatbot::Config config = chatbot_test::test_config();
    config.api_key.clear();
    config.base_url = "http://localhost:11434/v1";
    CHECK_FALSE(chatbot::validate_config(config).has_value());
    config.base_url = "https://api.openai.com/v1";
    const std::optional<chatbot::ChatError> error = chatbot::validate_config(config);
    REQUIRE(error.has_value());
    CHECK(error->kind == ErrorKind::Config);
    CHECK_THAT(error->message, Catch::Matchers::ContainsSubstring("CHAT_API_KEY"));
}

TEST_CASE("list_models: GET a {base_url}/models y lista ordenada", "[proveedor][modelos]") {
    chatbot::Config config = chatbot_test::test_config();
    config.base_url = "https://pruebas.invalid/v1/"; // Sin "/" duplicada.
    config.model.clear();                           // No hace falta para listar.
    auto harness = chatbot_test::make_client(config);
    harness.transport->responses.push_back(response(
        200, R"({"object":"list","data":[{"id":"models/gemini-b"},{"id":"zeta"},{"id":"alfa"},)"
             R"({"id":"gemini-b"},{"id":42},{"nombre":"sin id"},"texto",{"id":"zeta"}]})"));

    const auto models = harness.client->list_models();
    REQUIRE(models.is_ok());
    CHECK(models.value() == std::vector<std::string>{"alfa", "gemini-b", "zeta"});
    REQUIRE(harness.transport->requests.size() == 1);
    const chatbot::HttpRequest& request = harness.transport->requests.front();
    CHECK(request.method == chatbot::HttpMethod::Get);
    CHECK(request.url == "https://pruebas.invalid/v1/models");
    CHECK(request.body.empty());
    CHECK(request.api_key == config.api_key);
}

TEST_CASE("list_models: sin data, JSON inválido, 401 y local sin key", "[proveedor][modelos]") {
    for (const char* body : {R"({"object":"list"})", R"({"data":{"id":"x"}})", "[]", "no es json"}) {
        INFO(body);
        auto harness = chatbot_test::make_client();
        harness.transport->responses.push_back(response(200, body));
        const auto models = harness.client->list_models();
        REQUIRE(models.is_error());
        CHECK(models.error().kind == ErrorKind::BadResponse);
    }

    auto unauthorized = chatbot_test::make_client();
    unauthorized.transport->responses.push_back(
        response(401, R"({"error":{"message":"Invalid API key"}})"));
    const auto auth = unauthorized.client->list_models();
    REQUIRE(auth.is_error());
    CHECK(auth.error().kind == ErrorKind::Auth);
    CHECK(unauthorized.transport->requests.size() == 1); // Auth no se reintenta.

    chatbot::Config local = chatbot_test::test_config();
    local.api_key.clear();
    local.base_url = "http://localhost:11434/v1";
    auto ollama = chatbot_test::make_client(local);
    ollama.transport->responses.push_back(response(200, R"({"data":[{"id":"llama3"}]})"));
    const auto models = ollama.client->list_models();
    REQUIRE(models.is_ok());
    CHECK(ollama.transport->requests.front().api_key.empty());

    chatbot::Config remote = chatbot_test::test_config();
    remote.api_key.clear();
    auto missing = chatbot_test::make_client(remote);
    const auto no_key = missing.client->list_models();
    REQUIRE(no_key.is_error());
    CHECK(no_key.error().kind == ErrorKind::Config);
    CHECK(missing.transport->requests.empty());
}

TEST_CASE("list_models: reintenta un 503 y respeta la cancelación", "[proveedor][modelos]") {
    auto harness = chatbot_test::make_client();
    harness.transport->responses.push_back(response(503, "ocupado"));
    harness.transport->responses.push_back(response(200, R"({"data":[{"id":"m"}]})"));
    const auto models = harness.client->list_models();
    REQUIRE(models.is_ok());
    CHECK(harness.transport->requests.size() == 2);
    CHECK(harness.sleeper->sleeps.size() == 1);

    auto cancelled = chatbot_test::make_client();
    cancelled.transport->responses.push_back(response(200, R"({"data":[]})"));
    chatbot::CancelToken token;
    token.cancel();
    const auto result = cancelled.client->list_models(&token);
    REQUIRE(result.is_error());
    CHECK(result.error().kind == ErrorKind::Cancelled);
    CHECK(cancelled.transport->requests.empty());
}

TEST_CASE("credentials.json: ida y vuelta, permisos 0600 y carpeta 0700", "[proveedor][credenciales]") {
    const ScopedTempDir dir;
    const std::filesystem::path path = dir.path() / "chatbot" / "credentials.json";
    chatbot::Credentials credentials;
    credentials.keys[chatbot::credentials_key("nvidia", "https://integrate.api.nvidia.com/v1")] =
        kSecret;
    credentials.keys[chatbot::credentials_key("custom", "https://mi-servidor/v1/")] = "otra";
    REQUIRE_FALSE(chatbot::save_credentials(path.string(), credentials).has_value());
    CHECK(mode_of(path) == 0600);
    CHECK(mode_of(path.parent_path()) == 0700);

    const auto loaded = chatbot::load_credentials(path.string());
    REQUIRE(loaded.is_ok());
    CHECK(loaded.value().keys.at("nvidia") == kSecret);
    CHECK(loaded.value().keys.at("custom:https://mi-servidor/v1") == "otra");
    CHECK(chatbot::credentials_key("custom", "https://mi-servidor/v1") ==
          "custom:https://mi-servidor/v1");

    // Sin archivo: sin keys, sin error.
    const auto missing = chatbot::load_credentials((dir.path() / "no-existe.json").string());
    REQUIRE(missing.is_ok());
    CHECK(missing.value().keys.empty());
}

TEST_CASE("credentials.json con permisos abiertos no se lee", "[proveedor][credenciales]") {
    const ScopedTempDir dir;
    const std::filesystem::path path = dir.path() / "credentials.json";
    for (const mode_t mode : {mode_t{0640}, mode_t{0604}, mode_t{0660}, mode_t{0644}}) {
        INFO(std::oct << mode);
        write(path, std::string{R"({"version":1,"keys":{"nvidia":")"} + kSecret + "\"}}", mode);
        const auto loaded = chatbot::load_credentials(path.string());
        REQUIRE(loaded.is_error());
        CHECK(loaded.error().kind == ErrorKind::Config);
        CHECK(loaded.error().message ==
              "credentials.json tiene permisos demasiado abiertos; corre chmod 600 " +
                  path.string());
        CHECK(loaded.error().message.find(kSecret) == std::string::npos);
    }
    ::chmod(path.c_str(), 0600);
    CHECK(chatbot::load_credentials(path.string()).is_ok());

    // JSON inválido: error sin citar el contenido.
    write(path, std::string{"{ \"keys\": \""} + kSecret, 0600);
    const auto broken = chatbot::load_credentials(path.string());
    REQUIRE(broken.is_error());
    CHECK(broken.error().message.find(kSecret) == std::string::npos);
}

TEST_CASE("load_config: CHAT_API_KEY gana; si no, la key del proveedor", "[proveedor][credenciales]") {
    const ScopedTempDir dir;
    const std::filesystem::path config = dir.path() / "chatbot" / "config.json";
    write(config,
          R"({"provider":"nvidia","base_url":"https://integrate.api.nvidia.com/v1","model":"m"})",
          0644);
    chatbot::Credentials credentials;
    credentials.keys["nvidia"] = kSecret;
    credentials.keys["openai"] = "de-otro-proveedor";
    REQUIRE_FALSE(chatbot::save_credentials((config.parent_path() / "credentials.json").string(),
                                            credentials)
                      .has_value());

    FakeEnv env;
    const auto from_file = load(env, config);
    REQUIRE(from_file.is_ok());
    CHECK(from_file.value().api_key == kSecret);
    CHECK(from_file.value().provider == "nvidia");

    env.values["CHAT_API_KEY"] = "de-entorno";
    const auto from_env = load(env, config);
    REQUIRE(from_env.is_ok());
    CHECK(from_env.value().api_key == "de-entorno");

    // Proveedor personalizado: la key es la de su URL base.
    write(config, R"({"provider":"custom","base_url":"https://mi-servidor/v1/","model":"m"})", 0644);
    credentials.keys["custom:https://mi-servidor/v1"] = "de-mi-servidor";
    REQUIRE_FALSE(chatbot::save_credentials((config.parent_path() / "credentials.json").string(),
                                            credentials)
                      .has_value());
    FakeEnv empty;
    const auto custom = load(empty, config);
    REQUIRE(custom.is_ok());
    CHECK(custom.value().api_key == "de-mi-servidor");

    // credentials.json con permisos abiertos: error Config al arrancar.
    ::chmod((config.parent_path() / "credentials.json").c_str(), 0644);
    const auto open = load(empty, config);
    REQUIRE(open.is_error());
    CHECK(open.error().kind == ErrorKind::Config);
}

TEST_CASE("save_config_file conserva llaves y nunca escribe la key", "[proveedor][config]") {
    const ScopedTempDir dir;
    const std::filesystem::path config = dir.path() / "chatbot" / "config.json";
    write(config,
          R"({"timeout_seconds": 90, "history_limit": 1000, "otra": {"a": [1, 2]}, "model": "viejo"})",
          0640);
    chatbot::Credentials credentials;
    credentials.keys["openai"] = kSecret;
    REQUIRE_FALSE(chatbot::save_credentials((config.parent_path() / "credentials.json").string(),
                                            credentials)
                      .has_value());
    REQUIRE_FALSE(chatbot::save_config_file(
                      config.string(), {"openai", "https://api.openai.com/v1", "gpt-nuevo"})
                      .has_value());

    const std::string text = read(config);
    CHECK(text.find(kSecret) == std::string::npos);
    CHECK(text.find("api_key") == std::string::npos);
    CHECK(mode_of(config) == 0640); // Conserva los permisos que tenía.
    // Las llaves que ya estaban siguen, en su orden; las nuevas, al final.
    CHECK(text.find("\"timeout_seconds\": 90") < text.find("\"history_limit\": 1000"));
    CHECK(text.find("\"otra\"") != std::string::npos);
    CHECK(text.find("\"model\": \"gpt-nuevo\"") != std::string::npos);
    CHECK(text.find("\"provider\": \"openai\"") != std::string::npos);
    CHECK(text.find("\"base_url\": \"https://api.openai.com/v1\"") != std::string::npos);

    FakeEnv env;
    const auto loaded = load(env, config);
    REQUIRE(loaded.is_ok());
    CHECK(loaded.value().model == "gpt-nuevo");
    CHECK(loaded.value().timeout_seconds == std::chrono::seconds{90});
    CHECK(loaded.value().history_limit_bytes == 1000);
    CHECK(loaded.value().api_key == kSecret);

    // Sin archivo: se crea con 0644; con un archivo que no es JSON, no se toca.
    const std::filesystem::path fresh = dir.path() / "otro" / "config.json";
    REQUIRE_FALSE(chatbot::save_config_file(fresh.string(), {"ollama", "http://localhost:11434/v1", "llama3"})
                      .has_value());
    CHECK(mode_of(fresh) == 0644);
    write(config, "{ roto", 0644);
    CHECK(chatbot::save_config_file(config.string(), {"openai", "https://api.openai.com/v1", "x"})
              .has_value());
    CHECK(read(config) == "{ roto");
}
