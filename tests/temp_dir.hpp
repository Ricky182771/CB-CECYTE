#ifndef CHATBOT_TEST_TEMP_DIR_HPP
#define CHATBOT_TEST_TEMP_DIR_HPP

#include <algorithm>
#include <filesystem>
#include <random>
#include <stdexcept>
#include <string>
#include <system_error>

namespace chatbot_test {

/// Directorio temporal propio de la prueba; se borra completo al salir del
/// ámbito. Las pruebas nunca escriben fuera de él. Portable (sin mkdtemp ni
/// getpid): el nombre lleva un número al azar y create_directory falla si ya
/// existe, así que dos procesos nunca comparten carpeta.
class ScopedTempDir {
public:
    ScopedTempDir() : path_{create()} {}
    ~ScopedTempDir() {
        std::error_code ignored;
        std::filesystem::permissions(path_, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::add, ignored);
        std::filesystem::remove_all(path_, ignored);
    }
    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const { return path_; }
    [[nodiscard]] std::string string() const { return path_.string(); }

private:
    static std::filesystem::path create() {
        static std::mt19937_64 generator{std::random_device{}()};
        const std::filesystem::path base = std::filesystem::temp_directory_path();
        for (int attempt = 0; attempt < 100; ++attempt) {
            const std::filesystem::path candidate =
                base / ("chatbot_store_" + std::to_string(generator()));
            if (std::filesystem::create_directory(candidate)) {
                return candidate;
            }
        }
        throw std::runtime_error("no se pudo crear un directorio temporal");
    }

    std::filesystem::path path_;
};

/// Ruta esperada con el separador del sistema: en Windows, cada "/" pasa a
/// "\\" (como arma las rutas el programa); en POSIX queda igual.
[[nodiscard]] inline std::string native_separators(std::string path) {
#ifdef _WIN32
    std::replace(path.begin(), path.end(), '/', '\\');
#endif
    return path;
}

} // namespace chatbot_test

#endif // CHATBOT_TEST_TEMP_DIR_HPP
