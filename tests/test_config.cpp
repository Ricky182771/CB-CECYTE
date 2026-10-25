#include "chatbot/config.h"
#include "config_internal.h"
#include "env_guard.hpp"
#include "fake_transport.hpp"
#include "posix_permissions.hpp"
#include "temp_dir.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cerrno>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <utility>

#ifndef _WIN32
#include <unistd.h>
#endif

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
    CHECK(build_config_path(std::nullopt, std::string{"/xdg/custom"}, std::string{"/home/usuario"})
              .value() == "/xdg/custom/chatbot/config.json");
    // Con barra final se normaliza.
    CHECK(build_config_path(std::nullopt, std::string{"/xdg/custom/"}, std::string{"/home/usuario"})
              .value() == "/xdg/custom/chatbot/config.json");

    // Ausente: se usa $HOME/.config/chatbot/config.json.
    CHECK(build_config_path(std::nullopt, std::nullopt, std::string{"/home/usuario"})
              .value() == "/home/usuario/.config/chatbot/config.json");
    // Presente pero vacío: cuenta como ausente.
    CHECK(build_config_path(std::nullopt, std::string{}, std::string{"/home/usuario"})
              .value() == "/home/usuario/.config/chatbot/config.json");

    // Sin XDG ni HOME: no hay ruta determinable.
    CHECK_FALSE(build_config_path(std::nullopt, std::nullopt, std::nullopt).has_value());
}

TEST_CASE("build_windows_config_path: AppData\\Roaming, si no APPDATA", "[config][windows]") {
    using chatbot::build_windows_config_path;
    const std::string roaming = "C:\\Users\\Ñandú\\AppData\\Roaming";

    // La carpeta que da Windows manda, aunque APPDATA diga otra cosa.
    CHECK(build_windows_config_path(std::nullopt, roaming, std::string{"D:\\otra"}).value() ==
          roaming + "\\chatbot\\config.json");
    // Con barra final (\ o /) no se repite el separador.
    CHECK(build_windows_config_path(std::nullopt, roaming + "\\", std::nullopt).value() ==
          roaming + "\\chatbot\\config.json");
    CHECK(build_windows_config_path(std::nullopt, std::string{"C:/Users/Ana/AppData/Roaming/"},
                                    std::nullopt)
              .value() == "C:/Users/Ana/AppData/Roaming\\chatbot\\config.json");

    // Si Windows no la dio (o vacía), la variable APPDATA.
    CHECK(build_windows_config_path(std::nullopt, std::nullopt, roaming).value() ==
          roaming + "\\chatbot\\config.json");
    CHECK(build_windows_config_path(std::nullopt, std::string{}, roaming).value() ==
          roaming + "\\chatbot\\config.json");

    // Sin ninguna: no hay ruta. HOME y XDG_* ni siquiera son parámetros.
    CHECK_FALSE(build_windows_config_path(std::nullopt, std::nullopt, std::nullopt).has_value());
    CHECK_FALSE(build_windows_config_path(std::nullopt, std::string{}, std::string{}).has_value());
}

TEST_CASE("portable_data_dir: datos/ junto al ejecutable solo con chatbot.portable",
          "[config][portable]") {
    using chatbot::Os;
    using chatbot::portable_data_dir;
    CHECK(chatbot::kPortableMarker == "chatbot.portable");

    // Linux: con el marcador, <carpeta>/datos; sin barras repetidas.
    CHECK(portable_data_dir(std::string{"/opt/chatbot"}, true, Os::Posix) == "/opt/chatbot/datos");
    CHECK(portable_data_dir(std::string{"/opt/chatbot/"}, true, Os::Posix) ==
          "/opt/chatbot/datos");
    CHECK(portable_data_dir(std::string{"/"}, true, Os::Posix) == "/datos");
    // Windows: con barra invertida, también si la carpeta trae / o \ al final.
    CHECK(portable_data_dir(std::string{"E:\\chatbot"}, true, Os::Windows) == "E:\\chatbot\\datos");
    CHECK(portable_data_dir(std::string{"E:\\chatbot\\"}, true, Os::Windows) ==
          "E:\\chatbot\\datos");
    CHECK(portable_data_dir(std::string{"C:\\Users\\Ñandú\\Escritorio\\chatbot"}, true,
                            Os::Windows) == "C:\\Users\\Ñandú\\Escritorio\\chatbot\\datos");

    // Sin el marcador, o sin saber dónde está el ejecutable: no es portable.
    CHECK_FALSE(portable_data_dir(std::string{"/opt/chatbot"}, false, Os::Posix).has_value());
    CHECK_FALSE(portable_data_dir(std::string{"E:\\chatbot"}, false, Os::Windows).has_value());
    CHECK_FALSE(portable_data_dir(std::nullopt, true, Os::Posix).has_value());
    CHECK_FALSE(portable_data_dir(std::string{}, true, Os::Windows).has_value());
}

TEST_CASE("build_config_path: el modo portable gana sobre XDG_CONFIG_HOME y HOME",
          "[config][portable]") {
    using chatbot::build_config_path;
    const std::optional<std::string> portable =
        chatbot::portable_data_dir(std::string{"/media/usb/chatbot"}, true, chatbot::Os::Posix);
    CHECK(build_config_path(portable, std::string{"/xdg"}, std::string{"/home/usuario"}) ==
          "/media/usb/chatbot/datos/config.json");
    CHECK(build_config_path(portable, std::nullopt, std::nullopt) ==
          "/media/usb/chatbot/datos/config.json");
    // Sin chatbot.portable, las reglas de siempre.
    const std::optional<std::string> not_portable =
        chatbot::portable_data_dir(std::string{"/media/usb/chatbot"}, false, chatbot::Os::Posix);
    CHECK(build_config_path(not_portable, std::string{"/xdg"}, std::string{"/home/usuario"}) ==
          "/xdg/chatbot/config.json");
}

TEST_CASE("build_windows_config_path: el modo portable gana sobre AppData y APPDATA",
          "[config][portable][windows]") {
    using chatbot::build_windows_config_path;
    const std::string roaming = "C:\\Users\\Ana\\AppData\\Roaming";
    const std::optional<std::string> portable =
        chatbot::portable_data_dir(std::string{"E:\\chatbot"}, true, chatbot::Os::Windows);
    CHECK(build_windows_config_path(portable, roaming, roaming) ==
          "E:\\chatbot\\datos\\config.json");
    CHECK(build_windows_config_path(portable, std::nullopt, std::nullopt) ==
          "E:\\chatbot\\datos\\config.json");
    const std::optional<std::string> not_portable =
        chatbot::portable_data_dir(std::string{"E:\\chatbot"}, false, chatbot::Os::Windows);
    CHECK(build_windows_config_path(not_portable, roaming, std::nullopt) ==
          roaming + "\\chatbot\\config.json");
}

TEST_CASE("process_portable_data_dir: sin chatbot.portable junto a las pruebas no es portable",
          "[config][portable]") {
    // Nadie pone chatbot.portable junto a chatbot_tests: las rutas del
    // proceso de pruebas son las de siempre.
    CHECK_FALSE(chatbot::process_portable_data_dir().has_value());
    const std::optional<std::string> executable = chatbot::executable_dir();
    REQUIRE(executable.has_value());
    CHECK(std::filesystem::exists(std::filesystem::path{*executable}));
}

TEST_CASE("prepare_portable_data_dir: crea datos/ y no deja el archivo de prueba",
          "[config][portable]") {
    const chatbot_test::ScopedTempDir temp;
    const std::filesystem::path dir = temp.path() / "datos";
    CHECK_FALSE(chatbot::prepare_portable_data_dir(dir.string()).has_value());
    REQUIRE(std::filesystem::is_directory(dir));
    CHECK(std::filesystem::is_empty(dir));
    // Si ya existe, tampoco hay error ni basura.
    CHECK_FALSE(chatbot::prepare_portable_data_dir(dir.string()).has_value());
    CHECK(std::filesystem::is_empty(dir));
#ifndef _WIN32
    CHECK((std::filesystem::status(dir).permissions() & std::filesystem::perms::all) ==
          std::filesystem::perms::owner_all);
#endif
}

#ifndef _WIN32
namespace {

/// Cuántas veces aparece needle en text (sin traslapes).
std::size_t occurrences(const std::string& text, const std::string& needle) {
    std::size_t count = 0;
    for (std::size_t at = text.find(needle); at != std::string::npos;
         at = text.find(needle, at + needle.size())) {
        ++count;
    }
    return count;
}

} // namespace

TEST_CASE("prepare_portable_data_dir: carpeta de solo lectura da un error claro",
          "[config][portable]") {
    if (::geteuid() == 0) {
        SKIP("root ignora los permisos de archivo");
    }
    const chatbot_test::ScopedTempDir temp;
    // La carpeta del usuario, para comprobar que no se toca.
    const std::filesystem::path home = temp.path() / "home";
    std::filesystem::create_directory(home);
    const chatbot_test::EnvGuard home_guard("HOME", home.c_str());
    const chatbot_test::EnvGuard xdg_guard("XDG_CONFIG_HOME", nullptr);
    const chatbot_test::EnvGuard data_guard("XDG_DATA_HOME", nullptr);

    SECTION("la carpeta del programa es de solo lectura: datos/ no se puede crear") {
        const std::filesystem::path app = temp.path() / "app";
        std::filesystem::create_directory(app);
        chatbot_test::set_mode(app, 0555);
        const std::string dir = (app / "datos").string();
        const std::optional<chatbot::ChatError> error = chatbot::prepare_portable_data_dir(dir);
        REQUIRE(error.has_value());
        CHECK(error->kind == chatbot::ErrorKind::Config);
        CHECK_THAT(error->message, Catch::Matchers::StartsWith("Modo portable (chatbot.portable"));
        CHECK_THAT(error->message, Catch::Matchers::ContainsSubstring(
                                       "no se pudo crear la carpeta de datos " + dir + ": "));
        CHECK(occurrences(error->message, dir) == 1); // La ruta, una sola vez.
        CHECK_THAT(error->message, Catch::Matchers::ContainsSubstring("borra chatbot.portable"));
        CHECK_FALSE(std::filesystem::exists(app / "datos"));
        chatbot_test::set_mode(app, 0755);
    }
    SECTION("datos/ existe pero es de solo lectura") {
        const std::filesystem::path dir = temp.path() / "datos";
        std::filesystem::create_directory(dir);
        chatbot_test::set_mode(dir, 0555);
        const std::optional<chatbot::ChatError> error =
            chatbot::prepare_portable_data_dir(dir.string());
        REQUIRE(error.has_value());
        CHECK_THAT(error->message,
                   Catch::Matchers::ContainsSubstring(
                       "no se puede escribir en la carpeta de datos " + dir.string() + ": "));
        CHECK(occurrences(error->message, dir.string()) == 1); // La ruta, una sola vez.
        CHECK_THAT(error->message, !Catch::Matchers::ContainsSubstring(".prueba-escritura"));
        chatbot_test::set_mode(dir, 0755);
        CHECK(std::filesystem::is_empty(dir));
    }
    // Ni ~/.config ni ~/.local: nunca se cae en silencio a la carpeta del usuario.
    CHECK(std::filesystem::is_empty(home));
}
#endif

TEST_CASE("missing_config_dir_message dice qué faltó en cada sistema", "[config][windows]") {
    CHECK(chatbot::missing_config_dir_message(chatbot::Os::Posix) ==
          "No se encontró la carpeta de configuración (define HOME o XDG_CONFIG_HOME).");
    const std::string windows = chatbot::missing_config_dir_message(chatbot::Os::Windows);
    CHECK_THAT(windows, Catch::Matchers::ContainsSubstring("AppData\\Roaming"));
    CHECK_THAT(windows, Catch::Matchers::ContainsSubstring("APPDATA"));
    CHECK(windows.find("HOME") == std::string::npos);
}

TEST_CASE("default_config_path usa el entorno real del proceso", "[config][xdg]") {
    const std::optional<std::string> path = chatbot::default_config_path();
    if (chatbot::current_os() == chatbot::Os::Windows) {
        // En Windows: %APPDATA%\chatbot\config.json, sin mirar HOME.
        REQUIRE(path.has_value());
        REQUIRE_THAT(*path, Catch::Matchers::EndsWith("\\chatbot\\config.json"));
        return;
    }
    // El proceso de pruebas corre con HOME definido; basta con que la función
    // devuelva una ruta coherente dentro de él.
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

TEST_CASE("base_url debe usar https://", "[config][https]") {
    for (const char* url : {"http://integrate.api.nvidia.com/v1", "htps://localhost/v1", ""}) {
        INFO("base_url = \"" << url << "\"");
        chatbot::Config config = chatbot_test::test_config();
        config.base_url = url;

        const std::optional<chatbot::ChatError> error = chatbot::validate_config(config);
        REQUIRE(error.has_value());
        CHECK(error->kind == chatbot::ErrorKind::Config);
        CHECK_THAT(error->message, Catch::Matchers::ContainsSubstring("https://"));

        // El cliente construido a mano también lo rechaza sin llamar al transporte.
        auto harness = chatbot_test::make_client(config);
        const chatbot::Result<std::string> response =
            harness.client->complete(chatbot_test::sample_messages());
        REQUIRE(response.is_error());
        CHECK(response.error().kind == chatbot::ErrorKind::Config);
        const chatbot::Result<chatbot::CompletionInfo> streamed = harness.client->complete_stream(
            chatbot_test::sample_messages(), [](std::string_view) { return true; });
        REQUIRE(streamed.is_error());
        CHECK(streamed.error().kind == chatbot::ErrorKind::Config);
        CHECK(harness.transport->requests.empty());
    }
}

TEST_CASE("base_url con HTTPS:// en mayúsculas es válida", "[config][https]") {
    chatbot::Config config = chatbot_test::test_config();
    config.base_url = "HTTPS://pruebas.invalid/v1";
    CHECK_FALSE(chatbot::validate_config(config).has_value());

    auto harness = chatbot_test::make_client(config);
    harness.transport->responses.push_back(
        chatbot::HttpResponse{200, chatbot_test::success_body(), std::nullopt, false, ""});
    const chatbot::Result<std::string> response =
        harness.client->complete(chatbot_test::sample_messages());
    CHECK(response.is_ok());
    CHECK(harness.transport->requests.size() == 1);
}

TEST_CASE("CHAT_BASE_URL con http:// a otro host en load_config: error Config", "[config][https]") {
    // http:// solo se acepta en la propia máquina (localhost, 127.0.0.1, [::1]).
    chatbot_test::FakeEnv env;
    env.values["CHAT_API_KEY"] = "clave-ficticia";
    env.values["CHAT_MODEL"] = "modelo-x";
    env.values["CHAT_BASE_URL"] = "http://192.168.1.10:8080/v1";

    const chatbot::Result<chatbot::Config> config =
        chatbot::load_config(ConfigOptions{env, path_of(std::nullopt)});

    REQUIRE(config.is_error());
    CHECK(config.error().kind == chatbot::ErrorKind::Config);
}

TEST_CASE("timeout: 3600 s es el máximo válido", "[config][timeout]") {
    chatbot_test::FakeEnv env;
    env.values["CHAT_API_KEY"] = "clave-ficticia";
    env.values["CHAT_MODEL"] = "modelo-x";
    env.values["CHAT_TIMEOUT"] = "3600";

    const chatbot::Result<chatbot::Config> config =
        chatbot::load_config(ConfigOptions{env, path_of(std::nullopt)});
    REQUIRE(config.is_ok());
    CHECK(config.value().timeout_seconds == std::chrono::seconds{3600});

    chatbot::Config manual = chatbot_test::test_config();
    manual.timeout_seconds = std::chrono::seconds{3600};
    CHECK_FALSE(chatbot::validate_config(manual).has_value());
}

TEST_CASE("timeout: 0 sigue siendo válido (sin límite)", "[config][timeout]") {
    chatbot::Config manual = chatbot_test::test_config();
    manual.timeout_seconds = std::chrono::seconds{0};
    CHECK_FALSE(chatbot::validate_config(manual).has_value());
}

TEST_CASE("timeout: 3601 desde CHAT_TIMEOUT es error Config", "[config][timeout]") {
    for (const char* value : {"3601", "99999999999999999"}) {
        INFO("CHAT_TIMEOUT = " << value);
        chatbot_test::FakeEnv env;
        env.values["CHAT_API_KEY"] = "clave-ficticia";
        env.values["CHAT_MODEL"] = "modelo-x";
        env.values["CHAT_TIMEOUT"] = value;

        const chatbot::Result<chatbot::Config> config =
            chatbot::load_config(ConfigOptions{env, path_of(std::nullopt)});
        REQUIRE(config.is_error());
        CHECK(config.error().kind == chatbot::ErrorKind::Config);
        CHECK_THAT(config.error().message, Catch::Matchers::ContainsSubstring("3600"));
    }
}

TEST_CASE("timeout: 3601 desde el archivo es error Config", "[config][timeout]") {
    TempFile archivo{"chatbot_tests_timeout_grande.json",
                     R"({"model": "modelo-archivo", "timeout_seconds": 3601})"};
    chatbot_test::FakeEnv env;
    env.values["CHAT_API_KEY"] = "clave-ficticia";

    const chatbot::Result<chatbot::Config> config =
        chatbot::load_config(ConfigOptions{env, path_of(archivo.path())});
    REQUIRE(config.is_error());
    CHECK(config.error().kind == chatbot::ErrorKind::Config);
    CHECK_THAT(config.error().message, Catch::Matchers::ContainsSubstring("3600"));
}

TEST_CASE("timeout: 3601 en un Config a mano es error Config sin llamar al transporte",
          "[config][timeout]") {
    chatbot::Config manual = chatbot_test::test_config();
    manual.timeout_seconds = std::chrono::seconds{3601};

    const std::optional<chatbot::ChatError> error = chatbot::validate_config(manual);
    REQUIRE(error.has_value());
    CHECK(error->kind == chatbot::ErrorKind::Config);
    CHECK_THAT(error->message, Catch::Matchers::ContainsSubstring("3600"));

    manual.timeout_seconds = std::chrono::seconds{99999999999999999};
    auto harness = chatbot_test::make_client(manual);
    const chatbot::Result<std::string> response =
        harness.client->complete(chatbot_test::sample_messages());
    REQUIRE(response.is_error());
    CHECK(response.error().kind == chatbot::ErrorKind::Config);
    CHECK(harness.transport->requests.empty());
}

TEST_CASE("history_limit: por defecto 32000", "[config][historial]") {
    chatbot_test::FakeEnv env;
    env.values["CHAT_API_KEY"] = "clave-ficticia";
    env.values["CHAT_MODEL"] = "modelo-x";

    const chatbot::Result<chatbot::Config> config =
        chatbot::load_config(ConfigOptions{env, path_of(std::nullopt)});
    REQUIRE(config.is_ok());
    CHECK(config.value().history_limit_bytes == 32000);
}

TEST_CASE("history_limit: desde el archivo y el entorno pisa al archivo", "[config][historial]") {
    TempFile archivo{"chatbot_tests_history.json",
                     R"({"model": "modelo-archivo", "history_limit": 1500})"};
    chatbot_test::FakeEnv env;
    env.values["CHAT_API_KEY"] = "clave-ficticia";

    const chatbot::Result<chatbot::Config> from_file =
        chatbot::load_config(ConfigOptions{env, path_of(archivo.path())});
    REQUIRE(from_file.is_ok());
    CHECK(from_file.value().history_limit_bytes == 1500);

    env.values["CHAT_HISTORY_LIMIT"] = "0";
    const chatbot::Result<chatbot::Config> from_env =
        chatbot::load_config(ConfigOptions{env, path_of(archivo.path())});
    REQUIRE(from_env.is_ok());
    CHECK(from_env.value().history_limit_bytes == 0);
}

TEST_CASE("history_limit: CHAT_HISTORY_LIMIT inválido es error Config", "[config][historial]") {
    for (const char* value : {"mucho", "-5", "12.5", "99999999999999999999999"}) {
        INFO("CHAT_HISTORY_LIMIT = " << value);
        chatbot_test::FakeEnv env;
        env.values["CHAT_API_KEY"] = "clave-ficticia";
        env.values["CHAT_MODEL"] = "modelo-x";
        env.values["CHAT_HISTORY_LIMIT"] = value;

        const chatbot::Result<chatbot::Config> config =
            chatbot::load_config(ConfigOptions{env, path_of(std::nullopt)});
        REQUIRE(config.is_error());
        CHECK(config.error().kind == chatbot::ErrorKind::Config);
        CHECK_THAT(config.error().message,
                   Catch::Matchers::ContainsSubstring("CHAT_HISTORY_LIMIT"));
    }
}

TEST_CASE("history_limit: valor inválido en el archivo es error Config", "[config][historial]") {
    for (const char* content : {R"({"model": "m", "history_limit": "mucho"})",
                                R"({"model": "m", "history_limit": -1})",
                                R"({"model": "m", "history_limit": 1.5})"}) {
        INFO("archivo: " << content);
        TempFile archivo{"chatbot_tests_history_invalido.json", content};
        chatbot_test::FakeEnv env;
        env.values["CHAT_API_KEY"] = "clave-ficticia";

        const chatbot::Result<chatbot::Config> config =
            chatbot::load_config(ConfigOptions{env, path_of(archivo.path())});
        REQUIRE(config.is_error());
        CHECK(config.error().kind == chatbot::ErrorKind::Config);
        CHECK_THAT(config.error().message, Catch::Matchers::ContainsSubstring("history_limit"));
    }
}

namespace {

std::string read_whole(const std::filesystem::path& path) {
    std::ifstream file{path, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

void write_whole(const std::filesystem::path& path, const std::string& content) {
    std::ofstream file{path, std::ios::binary};
    file << content;
}

} // namespace

TEST_CASE("load_config_section: la sección como texto, en el orden del archivo",
          "[config][seccion]") {
    const chatbot_test::ScopedTempDir temp;
    const std::string path = (temp.path() / "config.json").string();

    // Sin archivo: no hay sección, y no es un error.
    const auto missing = chatbot::load_config_section(path, "addons");
    REQUIRE(missing.is_ok());
    CHECK_FALSE(missing.value().has_value());

    write_whole(path, R"({"model": "m", "addons": {"zeta": 1, "alfa": {"b": 2, "a": 1}}})");
    const auto section = chatbot::load_config_section(path, "addons");
    REQUIRE(section.is_ok());
    REQUIRE(section.value().has_value());
    CHECK(*section.value() == R"({"zeta":1,"alfa":{"b":2,"a":1}})");
    const auto absent = chatbot::load_config_section(path, "otra");
    REQUIRE(absent.is_ok());
    CHECK_FALSE(absent.value().has_value());

    write_whole(path, "[1, 2]");
    const auto invalid = chatbot::load_config_section(path, "addons");
    REQUIRE(invalid.is_error());
    CHECK(invalid.error().kind == chatbot::ErrorKind::Config);
}

TEST_CASE("save_config_section: escribe y borra la sección sin tocar lo demás",
          "[config][seccion]") {
    const chatbot_test::ScopedTempDir temp;
    const std::string path = (temp.path() / "config.json").string();
    write_whole(path, R"({"provider": "nvidia", "zeta": true, "model": "m"})");

    REQUIRE_FALSE(chatbot::save_config_section(path, "addons", R"({"telegram": {"version": 1}})"));
    CHECK(read_whole(path) == "{\n"
                              "    \"provider\": \"nvidia\",\n"
                              "    \"zeta\": true,\n"
                              "    \"model\": \"m\",\n"
                              "    \"addons\": {\n"
                              "        \"telegram\": {\n"
                              "            \"version\": 1\n"
                              "        }\n"
                              "    }\n"
                              "}\n");

    // Reemplazar la sección no la mueve de lugar.
    REQUIRE_FALSE(chatbot::save_config_section(path, "zeta", "false"));
    CHECK(read_whole(path).find("\"zeta\": false,\n    \"model\"") != std::string::npos);

    REQUIRE_FALSE(chatbot::save_config_section(path, "addons", std::nullopt));
    CHECK(read_whole(path) == "{\n"
                              "    \"provider\": \"nvidia\",\n"
                              "    \"zeta\": false,\n"
                              "    \"model\": \"m\"\n"
                              "}\n");
}

TEST_CASE("save_config_section: con JSON inválido no toca el archivo", "[config][seccion]") {
    const chatbot_test::ScopedTempDir temp;
    const std::string path = (temp.path() / "config.json").string();
    const std::string original = R"({"model": "m"})";
    write_whole(path, original);
    const auto bad_value = chatbot::save_config_section(path, "addons", "{sin cerrar");
    REQUIRE(bad_value.has_value());
    CHECK(bad_value->kind == chatbot::ErrorKind::Config);
    CHECK(read_whole(path) == original);

    write_whole(path, "no es json");
    const auto bad_file = chatbot::save_config_section(path, "addons", "{}");
    REQUIRE(bad_file.has_value());
    CHECK(read_whole(path) == "no es json");
}
