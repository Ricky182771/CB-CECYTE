#ifndef CHATBOT_TEST_TEMP_DIR_HPP
#define CHATBOT_TEST_TEMP_DIR_HPP

#include <unistd.h>

#include <filesystem>
#include <string>
#include <system_error>

namespace chatbot_test {

/// Directorio temporal propio de la prueba; se borra completo al salir del
/// ámbito. Las pruebas nunca escriben fuera de él.
class ScopedTempDir {
public:
    ScopedTempDir()
        : path_{std::filesystem::temp_directory_path() /
                ("chatbot_store_" + std::to_string(static_cast<long>(::getpid())) + "_" +
                 std::to_string(next()))} {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
        std::filesystem::create_directories(path_);
    }
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
    static int next() {
        static int counter = 0;
        return ++counter;
    }
    std::filesystem::path path_;
};

} // namespace chatbot_test

#endif // CHATBOT_TEST_TEMP_DIR_HPP
