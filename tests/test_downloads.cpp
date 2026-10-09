#include "downloads.h"
#include "temp_dir.hpp"

#include <catch2/catch_test_macros.hpp>

#include <sys/stat.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

chatbot::cli::EnvLookup fake_env(std::map<std::string, std::string> values) {
    return [values = std::move(values)](std::string_view name) -> std::optional<std::string> {
        const auto found = values.find(std::string{name});
        return found != values.end() ? std::optional<std::string>{found->second} : std::nullopt;
    };
}

void write_text(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream(path) << text;
}

std::string read_text(const std::string& path) {
    std::ifstream file(path);
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

unsigned mode_of(const std::string& path) {
    struct stat info{};
    REQUIRE(::stat(path.c_str(), &info) == 0);
    return static_cast<unsigned>(info.st_mode) & 07777U;
}

/// true si mode no da ningún permiso fuera de allowed (la umask solo quita).
bool within(unsigned mode, unsigned allowed) { return (mode & ~allowed) == 0U; }

/// Cambia la umask del proceso mientras vive y luego la restaura.
class ScopedUmask {
public:
    explicit ScopedUmask(mode_t mask) : old_(::umask(mask)) {}
    ~ScopedUmask() { ::umask(old_); }
    ScopedUmask(const ScopedUmask&) = delete;
    ScopedUmask& operator=(const ScopedUmask&) = delete;

private:
    mode_t old_;
};

} // namespace

TEST_CASE("extensión por lenguaje", "[downloads]") {
    using chatbot::cli::extension_for;
    const std::map<std::string, std::string> table{
        {"cpp", ".cpp"},   {"c++", ".cpp"},      {"cc", ".cpp"},    {"c", ".c"},
        {"h", ".h"},       {"hpp", ".hpp"},      {"python", ".py"}, {"py", ".py"},
        {"bash", ".sh"},   {"sh", ".sh"},        {"shell", ".sh"},  {"js", ".js"},
        {"javascript", ".js"}, {"ts", ".ts"},    {"json", ".json"}, {"html", ".html"},
        {"css", ".css"},   {"java", ".java"},    {"cs", ".cs"},     {"csharp", ".cs"},
        {"rust", ".rs"},   {"rs", ".rs"},        {"go", ".go"},     {"sql", ".sql"},
        {"yaml", ".yaml"}, {"yml", ".yaml"},     {"md", ".md"},     {"markdown", ".md"},
    };
    for (const auto& [language, extension] : table) {
        INFO(language);
        CHECK(extension_for(language) == extension);
    }
    CHECK(extension_for("CPP") == ".cpp");
    CHECK(extension_for("Python") == ".py");
    CHECK(extension_for("") == ".txt");
    CHECK(extension_for("brainfuck") == ".txt");
    CHECK(extension_for("typescript") == ".txt"); // Solo lo que dice la tabla.
}

TEST_CASE("validate_file_name rechaza nombres peligrosos con un motivo", "[downloads]") {
    using chatbot::cli::validate_file_name;
    CHECK_FALSE(validate_file_name("suma.cpp").has_value());
    CHECK_FALSE(validate_file_name("suma").has_value());
    CHECK_FALSE(validate_file_name("año-2026.txt").has_value());
    CHECK_FALSE(validate_file_name(std::string(100, 'a')).has_value());

    const std::vector<std::string> bad_names{
        "../x", "a/b", "/etc/passwd", "a\\b", "..", "a..b", ".bashrc", ".", "a\nb", "a\tb",
        "a\x1b[2J", "a\x7f", "a\xc2\x9b", "a\xff", std::string(101, 'a'), ""};
    for (const std::string& bad : bad_names) {
        INFO(bad);
        const std::optional<std::string> reason = validate_file_name(bad);
        REQUIRE(reason.has_value());
        CHECK_FALSE(reason->empty());
    }
    CHECK(validate_file_name("a/b")->find("sin carpetas") != std::string::npos);
    CHECK(validate_file_name(".x")->find("empezar con") != std::string::npos);
    CHECK(validate_file_name(std::string(101, 'a'))->find("100 bytes") != std::string::npos);
    CHECK(validate_file_name("a\x01")->find("control") != std::string::npos);
}

TEST_CASE("validate_file_name aplica las reglas de Windows en todas las plataformas",
          "[downloads][windows]") {
    using chatbot::cli::validate_file_name;

    // Caracteres que Windows no acepta; ":" crearía un flujo alterno de NTFS.
    for (const char* bad : {"a<b.txt", "a>b", "suma.cpp:x", "a\"b", "a|b", "que?.md", "a*.cpp"}) {
        INFO(bad);
        const std::optional<std::string> reason = validate_file_name(bad);
        REQUIRE(reason.has_value());
        CHECK(reason->find("< > : \" | ? *") != std::string::npos);
    }

    // Nombres reservados, sin distinguir mayúsculas y también con extensión.
    for (const char* bad : {"con", "CON", "con.cpp", "Con.tar.gz", "prn.txt", "aux", "NUL.json",
                            "com1", "COM9.py", "lpt1.c", "Lpt9", "con .txt"}) {
        INFO(bad);
        const std::optional<std::string> reason = validate_file_name(bad);
        REQUIRE(reason.has_value());
        CHECK(reason->find("reservado en Windows") != std::string::npos);
    }
    // Se parecen, pero no son dispositivos.
    for (const char* good : {"console.txt", "contacto.cpp", "com.cpp", "com10.c", "lpt0.txt",
                             "nul-algo.md", "auxiliar.py", "mi-con.cpp", "año.con"}) {
        INFO(good);
        CHECK_FALSE(validate_file_name(good).has_value());
    }

    // Terminar en punto o en espacio.
    for (const char* bad : {"notas.", "notas ", "suma.cpp "}) {
        INFO(bad);
        const std::optional<std::string> reason = validate_file_name(bad);
        REQUIRE(reason.has_value());
        CHECK(reason->find("terminar en punto ni en espacio") != std::string::npos);
    }
    CHECK_FALSE(validate_file_name("con espacios.txt").has_value());
}

TEST_CASE("nombre del archivo del bloque", "[downloads]") {
    using chatbot::cli::block_file_name;
    CHECK(block_file_name("", 3, "cpp") == "bloque-3.cpp");
    CHECK(block_file_name("", 1, "") == "bloque-1.txt");
    CHECK(block_file_name("suma.cpp", 3, "python") == "suma.cpp");
    CHECK(block_file_name("suma", 3, "cpp") == "suma.cpp");
    CHECK(block_file_name("script", 2, "bash") == "script.sh");
    CHECK(block_file_name("notas", 2, "") == "notas.txt");
}

TEST_CASE("el contenido termina con un salto de línea", "[downloads]") {
    using chatbot::cli::with_final_newline;
    CHECK(with_final_newline("int a;\n") == "int a;\n");
    CHECK(with_final_newline("int a;") == "int a;\n");
    CHECK(with_final_newline("") == "\n");
}

TEST_CASE("XDG_DOWNLOAD_DIR de user-dirs.dirs", "[downloads]") {
    const chatbot_test::ScopedTempDir dir;
    const fs::path file = dir.path() / "user-dirs.dirs";
    using chatbot::cli::read_xdg_download_dir;

    CHECK_FALSE(read_xdg_download_dir((dir.path() / "no-existe").string(), "/home/a").has_value());

    write_text(file, "# Comentario\nXDG_DESKTOP_DIR=\"$HOME/Escritorio\"\n"
                     "XDG_DOWNLOAD_DIR=\"$HOME/Descargas\"\n");
    CHECK(read_xdg_download_dir(file.string(), "/home/a") == "/home/a/Descargas");

    write_text(file, "XDG_DOWNLOAD_DIR=\"/srv/descargas\"\n");
    CHECK(read_xdg_download_dir(file.string(), "/home/a") == "/srv/descargas");

    write_text(file, "XDG_DOWNLOAD_DIR=\"$HOME\"\n");
    CHECK(read_xdg_download_dir(file.string(), "/home/a") == "/home/a");

    for (const char* bad : {"XDG_DOWNLOAD_DIR=\"Descargas\"\n", "XDG_DOWNLOAD_DIR=$HOME/x\n",
                            "XDG_DOWNLOAD_DIR=\"$HOMEx/y\"\n", "XDG_DOWNLOAD_DIR=\"$OTRA/x\"\n",
                            "# XDG_DOWNLOAD_DIR=\"/x\"\n"}) {
        INFO(bad);
        write_text(file, bad);
        CHECK_FALSE(read_xdg_download_dir(file.string(), "/home/a").has_value());
    }
    // Sin HOME no se puede expandir.
    write_text(file, "XDG_DOWNLOAD_DIR=\"$HOME/Descargas\"\n");
    CHECK_FALSE(read_xdg_download_dir(file.string(), "").has_value());
}

TEST_CASE("carpeta de descargas en Windows: CHAT_DOWNLOAD_DIR, Descargas o USERPROFILE",
          "[downloads][windows]") {
    using chatbot::cli::resolve_windows_download_dir;
    const std::string downloads = "C:\\Users\\Ñandú\\Downloads";
    const std::string profile = "C:\\Users\\Ñandú";
    CHECK(resolve_windows_download_dir(std::string{"D:\\bajadas"}, downloads, profile) ==
          "D:\\bajadas");
    CHECK(resolve_windows_download_dir(std::string{""}, downloads, profile) == downloads);
    CHECK(resolve_windows_download_dir(std::nullopt, std::nullopt, profile) == profile);
    CHECK(resolve_windows_download_dir(std::nullopt, std::string{}, profile) == profile);
    CHECK_FALSE(resolve_windows_download_dir(std::nullopt, std::nullopt, std::nullopt).has_value());
}

TEST_CASE("avisos sin carpeta de descargas y home para mostrar rutas", "[downloads][windows]") {
    using chatbot::Os;
    CHECK(chatbot::cli::no_download_dir_message(Os::Posix) == chatbot::cli::kNoDownloadDir);
    const std::string_view windows = chatbot::cli::no_download_dir_message(Os::Windows);
    CHECK(windows.find("USERPROFILE") != std::string_view::npos);
    CHECK(windows.find("HOME") == std::string_view::npos);

    // En Windows las rutas se muestran completas: no hay home que abreviar.
    const auto env = fake_env({{"HOME", "/home/ana"}});
    CHECK(chatbot::cli::display_home(env, Os::Posix) == "/home/ana");
    CHECK(chatbot::cli::display_home(env, Os::Windows).empty());
    CHECK(chatbot::cli::display_home(fake_env({}), Os::Posix).empty());
}

TEST_CASE("carpeta de descargas, en orden de prioridad", "[downloads]") {
    using chatbot::cli::resolve_download_dir;
    const chatbot_test::ScopedTempDir dir;
    const std::string home = (dir.path() / "home").string();
    fs::create_directories(home);

    SECTION("Sin HOME ni CHAT_DOWNLOAD_DIR") {
        CHECK_FALSE(resolve_download_dir(fake_env({})).has_value());
    }

    SECTION("Solo HOME") {
        CHECK(resolve_download_dir(fake_env({{"HOME", home}})) == home);
    }

    SECTION("~/Downloads y ~/Descargas") {
        fs::create_directories(home + "/Downloads");
        CHECK(resolve_download_dir(fake_env({{"HOME", home}})) == home + "/Downloads");
        fs::create_directories(home + "/Descargas");
        CHECK(resolve_download_dir(fake_env({{"HOME", home}})) == home + "/Descargas");
    }

    SECTION("Termux: ~/storage/downloads si existe") {
        fs::create_directories(home + "/Descargas");
        CHECK(resolve_download_dir(fake_env({{"HOME", home}, {"TERMUX_VERSION", "0.118"}})) ==
              home + "/Descargas");
        fs::create_directories(home + "/storage/downloads");
        CHECK(resolve_download_dir(fake_env({{"HOME", home}, {"TERMUX_VERSION", "0.118"}})) ==
              home + "/storage/downloads");
        CHECK(resolve_download_dir(fake_env({{"HOME", home}})) == home + "/Descargas");
    }

    SECTION("user-dirs.dirs en ~/.config y en XDG_CONFIG_HOME") {
        fs::create_directories(home + "/Descargas");
        fs::create_directories(home + "/Bajadas");
        write_text(home + "/.config/user-dirs.dirs", "XDG_DOWNLOAD_DIR=\"$HOME/Bajadas\"\n");
        CHECK(resolve_download_dir(fake_env({{"HOME", home}})) == home + "/Bajadas");

        const std::string config = (dir.path() / "config").string();
        fs::create_directories(home + "/Otra");
        write_text(config + "/user-dirs.dirs", "XDG_DOWNLOAD_DIR=\"$HOME/Otra\"\n");
        CHECK(resolve_download_dir(fake_env({{"HOME", home}, {"XDG_CONFIG_HOME", config}})) ==
              home + "/Otra");
    }

    SECTION("XDG_DOWNLOAD_DIR que no existe: se sigue con el siguiente paso") {
        fs::create_directories(home + "/Descargas");
        write_text(home + "/.config/user-dirs.dirs", "XDG_DOWNLOAD_DIR=\"$HOME/NoExiste\"\n");
        CHECK(resolve_download_dir(fake_env({{"HOME", home}})) == home + "/Descargas");
    }

    SECTION("CHAT_DOWNLOAD_DIR gana a todo; vacía no cuenta") {
        fs::create_directories(home + "/Descargas");
        CHECK(resolve_download_dir(fake_env({{"HOME", home}, {"CHAT_DOWNLOAD_DIR", "/x/y"}})) ==
              "/x/y");
        CHECK(resolve_download_dir(fake_env({{"CHAT_DOWNLOAD_DIR", "/x/y"}})) == "/x/y");
        CHECK(resolve_download_dir(fake_env({{"HOME", home}, {"CHAT_DOWNLOAD_DIR", ""}})) ==
              home + "/Descargas");
    }
}

TEST_CASE("ensure_download_dir crea chatbot/ con 0755 o menos", "[downloads]") {
    const chatbot_test::ScopedTempDir dir;
    const chatbot::cli::WriteResult created = chatbot::cli::ensure_download_dir(dir.string());
    REQUIRE(created.error.empty());
    CHECK(created.path == dir.string() + "/chatbot");
    // La umask del usuario puede quitar permisos, nunca agregarlos.
    CHECK(within(mode_of(created.path), 0755U));
    CHECK((mode_of(created.path) & 0700U) == 0700U);
    // Si ya existe, se usa tal cual.
    CHECK(chatbot::cli::ensure_download_dir(dir.string()).error.empty());

    const chatbot_test::ScopedTempDir other;
    write_text(other.path() / "chatbot", "no soy carpeta");
    CHECK_FALSE(chatbot::cli::ensure_download_dir(other.string()).error.empty());
    CHECK_FALSE(
        chatbot::cli::ensure_download_dir((other.path() / "no-existe").string()).error.empty());
}

TEST_CASE("write_new_file nunca sobrescribe y deja 0644 o menos", "[downloads]") {
    using chatbot::cli::write_new_file;
    const chatbot_test::ScopedTempDir dir;

    const auto first = write_new_file(dir.string(), "suma.cpp", "uno\n");
    REQUIRE(first.error.empty());
    CHECK(first.path == dir.string() + "/suma.cpp");
    CHECK(read_text(first.path) == "uno\n");
    CHECK(within(mode_of(first.path), 0644U));
    CHECK((mode_of(first.path) & 0600U) == 0600U);

    const auto second = write_new_file(dir.string(), "suma.cpp", "dos\n");
    REQUIRE(second.error.empty());
    CHECK(second.path == dir.string() + "/suma-2.cpp");
    CHECK(read_text(first.path) == "uno\n"); // El primero no cambió.
    CHECK(read_text(second.path) == "dos\n");

    const auto third = write_new_file(dir.string(), "suma.cpp", "tres\n");
    CHECK(third.path == dir.string() + "/suma-3.cpp");

    // Sin extensión, el sufijo va al final.
    CHECK(write_new_file(dir.string(), "notas", "x").path == dir.string() + "/notas");
    CHECK(write_new_file(dir.string(), "notas", "x").path == dir.string() + "/notas-2");

    // Un .sh nunca queda ejecutable.
    const auto script = write_new_file(dir.string(), "script.sh", "echo hola\n");
    CHECK(within(mode_of(script.path), 0644U));
    CHECK((mode_of(script.path) & 0111U) == 0U);
}

TEST_CASE("la carpeta y los archivos respetan la umask del usuario", "[downloads]") {
    const chatbot_test::ScopedTempDir dir;
    const ScopedUmask strict(077);
    const chatbot::cli::WriteResult created = chatbot::cli::ensure_download_dir(dir.string());
    REQUIRE(created.error.empty());
    CHECK(mode_of(created.path) == 0700U);
    const auto written = chatbot::cli::write_new_file(created.path, "a.txt", "x\n");
    REQUIRE(written.error.empty());
    CHECK(mode_of(written.path) == 0600U);
}

TEST_CASE("write_new_file se rinde después de -99", "[downloads]") {
    using chatbot::cli::write_new_file;
    const chatbot_test::ScopedTempDir dir;
    write_text(dir.path() / "a.txt", "0");
    for (int i = 2; i <= chatbot::cli::kMaxNameSuffix; ++i) {
        write_text(dir.path() / ("a-" + std::to_string(i) + ".txt"), "0");
    }
    const auto result = write_new_file(dir.string(), "a.txt", "nuevo");
    CHECK(result.path.empty());
    CHECK(result.error.find("-99") != std::string::npos);
    CHECK(read_text((dir.path() / "a-99.txt").string()) == "0");
}

TEST_CASE("write_new_file no sigue enlaces simbólicos ni escribe en carpetas que no existen",
          "[downloads]") {
    using chatbot::cli::write_new_file;
    const chatbot_test::ScopedTempDir dir;
    write_text(dir.path() / "destino.txt", "original");
    fs::create_symlink(dir.path() / "destino.txt", dir.path() / "enlace.txt");
    const auto result = write_new_file(dir.string(), "enlace.txt", "nuevo");
    CHECK(result.path == dir.string() + "/enlace-2.txt"); // El enlace cuenta como existente.
    CHECK(read_text((dir.path() / "destino.txt").string()) == "original");

    CHECK_FALSE(write_new_file((dir.path() / "no-existe").string(), "a.txt", "x").error.empty());
}

TEST_CASE("display_path abrevia HOME con ~", "[downloads]") {
    using chatbot::cli::display_path;
    CHECK(display_path("/home/a/Descargas/chatbot/suma.cpp", "/home/a") ==
          "~/Descargas/chatbot/suma.cpp");
    CHECK(display_path("/home/ab/x", "/home/a") == "/home/ab/x");
    CHECK(display_path("/srv/x", "/home/a") == "/srv/x");
    CHECK(display_path("/srv/x", "") == "/srv/x");
}
