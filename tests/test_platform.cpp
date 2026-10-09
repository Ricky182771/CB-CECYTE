// Capa de plataforma (chatbot/platform.h): las mismas pruebas corren en
// Linux y en Windows; las de permisos POSIX, solo en POSIX.

#include "chatbot/platform.h"
#include "temp_dir.hpp"

#include <catch2/catch_test_macros.hpp>

#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace {

namespace fs = std::filesystem;

std::string read_file(const fs::path& path) {
    std::ifstream file{path, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

/// Nombres de lo que hay en dir (para revisar que no quede ningún .tmp).
std::vector<std::string> entries(const fs::path& dir) {
    std::vector<std::string> names;
    for (const fs::directory_entry& entry : fs::directory_iterator{dir}) {
        names.push_back(entry.path().filename().string());
    }
    return names;
}

#ifndef _WIN32
unsigned mode_of(const fs::path& path) {
    struct stat info {};
    REQUIRE(::stat(path.c_str(), &info) == 0);
    return static_cast<unsigned>(info.st_mode) & 0777U;
}
#endif

} // namespace

TEST_CASE("local_time: hora local de un instante y vuelta con mktime", "[plataforma]") {
    const std::time_t when = 1760000000; // 2025-10-09 08:53:20 UTC.
    const std::optional<std::tm> local = chatbot::local_time(when);
    REQUIRE(local.has_value());
    CHECK(local->tm_year == 2025 - 1900);
    CHECK(local->tm_mon == 9);
    CHECK((local->tm_mday == 8 || local->tm_mday == 9 || local->tm_mday == 10));
    std::tm copy = *local;
    CHECK(std::mktime(&copy) == when);

    const std::optional<std::tm> epoch = chatbot::local_time(0);
    REQUIRE(epoch.has_value());
    CHECK((epoch->tm_year == 69 || epoch->tm_year == 70));
}

TEST_CASE("write_file_atomic: ida y vuelta en una carpeta con ñ y acentos", "[plataforma]") {
    const chatbot_test::ScopedTempDir temp;
    // Rutas en UTF-8, como todo std::string del proyecto.
    const fs::path dir = temp.path() / fs::path{u8"Configuración del niño áéíóú"} / "chatbot";
    const std::string path = (dir / fs::path{u8"señal.json"}).string();

    using chatbot::FilePrivacy;
    using chatbot::FolderPrivacy;
    REQUIRE_FALSE(chatbot::write_file_atomic(path, "{\"a\": \"ñ\"}\n", FilePrivacy::Private,
                                             FolderPrivacy::Private)
                      .has_value());
    CHECK(read_file(fs::path{path}) == "{\"a\": \"ñ\"}\n");

    // Reemplazar un archivo que ya existe, sin dejar el .tmp.
    REQUIRE_FALSE(chatbot::write_file_atomic(path, "segundo", FilePrivacy::KeepExisting,
                                             FolderPrivacy::PrivateIfNew)
                      .has_value());
    CHECK(read_file(fs::path{path}) == "segundo");
    CHECK(entries(dir) == std::vector<std::string>{fs::path{path}.filename().string()});
}

TEST_CASE("write_file_atomic: si no puede escribir, da el error y no deja el .tmp",
          "[plataforma]") {
    const chatbot_test::ScopedTempDir temp;
    // La "carpeta" es un archivo: no se puede crear el .tmp dentro.
    const fs::path blocker = temp.path() / "archivo";
    std::ofstream{blocker} << "x";
    const std::string path = (blocker / "config.json").string();
    const std::optional<std::string> error = chatbot::write_file_atomic(
        path, "x", chatbot::FilePrivacy::Private, chatbot::FolderPrivacy::Private);
    REQUIRE(error.has_value());
    CHECK_FALSE(error->empty());
    CHECK(entries(temp.path()) == std::vector<std::string>{"archivo"});

    // El destino es una carpeta: no se puede reemplazar y el .tmp se borra.
    const fs::path folder = temp.path() / "carpeta";
    fs::create_directories(folder / "dentro");
    const std::optional<std::string> replace = chatbot::write_file_atomic(
        folder.string(), "x", chatbot::FilePrivacy::Private, chatbot::FolderPrivacy::Private);
    REQUIRE(replace.has_value());
    CHECK(replace->find("no se pudo reemplazar") != std::string::npos);
    CHECK_FALSE(fs::exists(temp.path() / "carpeta.tmp"));
}

#ifndef _WIN32
TEST_CASE("write_file_atomic: permisos POSIX de cada FilePrivacy y FolderPrivacy",
          "[plataforma][permisos]") {
    using chatbot::FilePrivacy;
    using chatbot::FolderPrivacy;
    const chatbot_test::ScopedTempDir temp;

    // Private: 0600 aunque el archivo existiera con otros; la carpeta, 0700.
    const fs::path private_dir = temp.path() / "privada";
    fs::create_directories(private_dir);
    ::chmod(private_dir.c_str(), 0755);
    const fs::path secret = private_dir / "credentials.json";
    std::ofstream{secret} << "viejo";
    ::chmod(secret.c_str(), 0644);
    REQUIRE_FALSE(chatbot::write_file_atomic(secret.string(), "x", FilePrivacy::Private,
                                             FolderPrivacy::Private)
                      .has_value());
    CHECK(mode_of(secret) == 0600U);
    CHECK(mode_of(private_dir) == 0700U);

    // KeepExisting: conserva los permisos; sin archivo, 0644.
    const fs::path config = private_dir / "config.json";
    std::ofstream{config} << "{}";
    ::chmod(config.c_str(), 0640);
    REQUIRE_FALSE(chatbot::write_file_atomic(config.string(), "{}", FilePrivacy::KeepExisting,
                                             FolderPrivacy::Private)
                      .has_value());
    CHECK(mode_of(config) == 0640U);
    const fs::path fresh = private_dir / "nuevo.json";
    REQUIRE_FALSE(chatbot::write_file_atomic(fresh.string(), "{}", FilePrivacy::KeepExisting,
                                             FolderPrivacy::Private)
                      .has_value());
    CHECK(mode_of(fresh) == 0644U);

    // PrivateIfNew: una carpeta nueva queda en 0700; una que ya existía, igual.
    const fs::path shared = temp.path() / "compartida";
    fs::create_directories(shared);
    ::chmod(shared.c_str(), 0755);
    REQUIRE_FALSE(chatbot::write_file_atomic((shared / "a.json").string(), "x",
                                             FilePrivacy::Private, FolderPrivacy::PrivateIfNew)
                      .has_value());
    CHECK(mode_of(shared) == 0755U);
    const fs::path created = temp.path() / "nueva";
    REQUIRE_FALSE(chatbot::write_file_atomic((created / "a.json").string(), "x",
                                             FilePrivacy::Private, FolderPrivacy::PrivateIfNew)
                      .has_value());
    CHECK(mode_of(created) == 0700U);
}
#endif

TEST_CASE("join_windows_path no repite el separador", "[plataforma][windows]") {
    using chatbot::join_windows_path;
    CHECK(join_windows_path("C:\\Users\\Ana", "chatbot") == "C:\\Users\\Ana\\chatbot");
    CHECK(join_windows_path("C:\\Users\\Ana\\", "chatbot") == "C:\\Users\\Ana\\chatbot");
    CHECK(join_windows_path("C:/Users/Ana/", "a\\b") == "C:/Users/Ana\\a\\b");
    CHECK(join_windows_path("D:\\", "x") == "D:\\x");
}

TEST_CASE("known_folder: las carpetas de Windows; en POSIX no hay", "[plataforma][windows]") {
    using chatbot::KnownFolder;
    for (const KnownFolder folder :
         {KnownFolder::RoamingAppData, KnownFolder::LocalAppData, KnownFolder::Downloads}) {
        const std::optional<std::string> path = chatbot::known_folder(folder);
        if (chatbot::current_os() == chatbot::Os::Windows) {
            REQUIRE(path.has_value());
            CHECK_FALSE(path->empty());
        } else {
            CHECK_FALSE(path.has_value());
        }
    }
}

TEST_CASE("interactive_console_input_mode solo quita ENABLE_PROCESSED_INPUT",
          "[plataforma][windows]") {
    using chatbot::interactive_console_input_mode;
    // Valores de la documentación de SetConsoleMode.
    constexpr unsigned kProcessed = 0x0001;     // ENABLE_PROCESSED_INPUT
    constexpr unsigned kLine = 0x0002;          // ENABLE_LINE_INPUT
    constexpr unsigned kEcho = 0x0004;          // ENABLE_ECHO_INPUT
    constexpr unsigned kWindow = 0x0008;        // ENABLE_WINDOW_INPUT
    constexpr unsigned kMouse = 0x0010;         // ENABLE_MOUSE_INPUT
    constexpr unsigned kInsert = 0x0020;        // ENABLE_INSERT_MODE
    constexpr unsigned kQuickEdit = 0x0040;     // ENABLE_QUICK_EDIT_MODE
    constexpr unsigned kExtended = 0x0080;      // ENABLE_EXTENDED_FLAGS
    constexpr unsigned kAutoPosition = 0x0100; // ENABLE_AUTO_POSITION
    constexpr unsigned kVirtualTerminal = 0x0200; // ENABLE_VIRTUAL_TERMINAL_INPUT
    CHECK(chatbot::kConsoleProcessedInput == kProcessed);

    // El modo por defecto de conhost (0x1F7): todo igual menos el bit 0x1.
    constexpr unsigned kDefault =
        kProcessed | kLine | kEcho | kMouse | kInsert | kQuickEdit | kExtended | kAutoPosition;
    CHECK(kDefault == 0x1F7U);
    CHECK(interactive_console_input_mode(kDefault) == 0x1F6U);
    CHECK(interactive_console_input_mode(kProcessed) == 0U);
    CHECK(interactive_console_input_mode(0U) == 0U);
    // Lo que pone FTXUI y QuickEdit (W2) no se tocan.
    CHECK(interactive_console_input_mode(kVirtualTerminal | kWindow | kProcessed) ==
          (kVirtualTerminal | kWindow));
    CHECK(interactive_console_input_mode(kQuickEdit | kExtended) == (kQuickEdit | kExtended));
    // Aplicarla dos veces da lo mismo.
    CHECK(interactive_console_input_mode(interactive_console_input_mode(kDefault)) == 0x1F6U);
}

TEST_CASE("std::random_device no es determinista", "[plataforma]") {
    // En MinGW antes de GCC 9.2, random_device daba siempre la misma
    // secuencia. Dos instancias distintas no deben coincidir en 4 valores
    // de 32 bits (la probabilidad de que coincidan por azar es 2^-128).
    std::random_device first;
    std::random_device second;
    std::vector<unsigned> a;
    std::vector<unsigned> b;
    for (int i = 0; i < 4; ++i) {
        a.push_back(first());
        b.push_back(second());
    }
    CHECK(a != b);
}
