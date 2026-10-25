// invitados.json: lo escriben el bot (al aceptar un código) y la app (al
// quitar a alguien), siempre bajo flock y sin perder cambios.

#include "chatbot/addon_settings/invited.h"

#include "chatbot/platform.h"

#include "temp_dir.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace settings = chatbot::addon_settings;
using Users = std::vector<std::int64_t>;

TEST_CASE("invitados: en Windows no están disponibles", "[addon_settings][invitados]") {
    if (chatbot::current_os() != chatbot::Os::Windows) {
        SKIP("solo Windows");
    }
    const chatbot_test::ScopedTempDir dir;
    const std::string path = (dir.path() / "invitados.json").string();
    CHECK(settings::load_invited(path).is_error());
    CHECK(settings::add_invited(path, 5).is_error());
    CHECK(settings::remove_invited(path, 5).is_error());
}

#ifndef _WIN32

namespace {

void write(const std::filesystem::path& path, const std::string& text) {
    std::ofstream file{path, std::ios::binary};
    file << text;
}

std::string read(const std::filesystem::path& path) {
    std::ifstream file{path, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

} // namespace

TEST_CASE("invitados: se agregan y se quitan con 600 de forma atómica",
          "[addon_settings][invitados]") {
    const chatbot_test::ScopedTempDir dir;
    const std::filesystem::path path = dir.path() / "datos" / "invitados.json";
    // Leer no crea la carpeta.
    const auto empty = settings::load_invited(path.string());
    REQUIRE(empty.is_ok());
    CHECK(empty.value().empty());
    CHECK_FALSE(std::filesystem::exists(path.parent_path()));

    CHECK(settings::add_invited(path.string(), 5).value() == Users{5});
    CHECK(settings::add_invited(path.string(), 7).value() == Users{5, 7});
    CHECK(settings::add_invited(path.string(), 5).value() == Users{5, 7}); // Ya estaba.
    CHECK(settings::add_invited(path.string(), 9).value() == Users{5, 7, 9});
    CHECK(settings::remove_invited(path.string(), 7).value() == Users{5, 9});
    CHECK(settings::remove_invited(path.string(), 8).value() == Users{5, 9}); // No estaba.
    CHECK(settings::load_invited(path.string()).value() == Users{5, 9});

    // Solo IDs: nada de nombres ni mensajes.
    CHECK(nlohmann::json::parse(read(path)) ==
          nlohmann::json{{"version", 1}, {"users", {5, 9}}});
    CHECK_FALSE(std::filesystem::exists(path.string() + ".tmp"));
    struct stat info {};
    REQUIRE(::stat(path.c_str(), &info) == 0);
    CHECK((info.st_mode & 0777) == 0600);
    REQUIRE(::stat((path.string() + ".lock").c_str(), &info) == 0);
    CHECK((info.st_mode & 0077) == 0);
    REQUIRE(::stat(path.parent_path().c_str(), &info) == 0);
    CHECK((info.st_mode & 0777) == 0700);
}

TEST_CASE("invitados: un archivo dañado es un error claro y no se toca",
          "[addon_settings][invitados]") {
    for (const char* content : {"{ roto", "[1, 2]", "{\"version\": 2, \"users\": []}",
                                "{\"version\": 1}", "{\"version\": 1, \"users\": [\"5\"]}",
                                "{\"version\": 1, \"users\": [0]}"}) {
        const chatbot_test::ScopedTempDir dir;
        const std::filesystem::path path = dir.path() / "invitados.json";
        write(path, content);
        INFO(content);
        for (const auto& result : {settings::load_invited(path.string()),
                                   settings::add_invited(path.string(), 5),
                                   settings::remove_invited(path.string(), 5)}) {
            REQUIRE(result.is_error());
            CHECK(result.error().kind == chatbot::ErrorKind::BadResponse);
            CHECK(result.error().message.find(path.string()) != std::string::npos);
            CHECK(result.error().message.find("dañado") != std::string::npos);
        }
        CHECK(read(path) == content);
    }
}

TEST_CASE("invitados: el bot ya no pisa a quien quitó la app", "[addon_settings][invitados]") {
    const chatbot_test::ScopedTempDir dir;
    const std::string path = (dir.path() / "invitados.json").string();
    REQUIRE(settings::add_invited(path, 1).is_ok());
    REQUIRE(settings::add_invited(path, 2).is_ok());
    // La app quita al 1 mientras el bot sigue corriendo con {1, 2} en memoria.
    REQUIRE(settings::remove_invited(path, 1).is_ok());
    // El bot acepta al 3: agrega solo a ese y devuelve la lista del archivo.
    CHECK(settings::add_invited(path, 3).value() == Users{2, 3});
    CHECK(settings::load_invited(path).value() == Users{2, 3});
}

TEST_CASE("invitados: escrituras a la vez no pierden cambios", "[addon_settings][invitados]") {
    const chatbot_test::ScopedTempDir dir;
    const std::string path = (dir.path() / "invitados.json").string();
    constexpr int kThreads = 8;
    constexpr int kPerThread = 25;
    // El 1000 está desde antes y uno de los hilos lo quita a la mitad.
    REQUIRE(settings::add_invited(path, 1000).is_ok());
    std::vector<std::thread> threads;
    std::atomic<int> failures{0};
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&path, &failures, t] {
            for (int i = 0; i < kPerThread; ++i) {
                if (settings::add_invited(path, t * kPerThread + i + 1).is_error()) {
                    ++failures;
                }
                (void)settings::load_invited(path);
            }
        });
    }
    threads.emplace_back([&path, &failures] {
        if (settings::remove_invited(path, 1000).is_error()) {
            ++failures;
        }
    });
    for (std::thread& thread : threads) {
        thread.join();
    }
    CHECK(failures == 0);
    Users users = settings::load_invited(path).value();
    std::sort(users.begin(), users.end());
    Users expected;
    for (int i = 1; i <= kThreads * kPerThread; ++i) {
        expected.push_back(i);
    }
    CHECK(users == expected);
}

#endif
