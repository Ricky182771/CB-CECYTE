// Instrucciones de sistema: lectura y escritura de "system_prompt" en
// config.json (núcleo) y su validación y resolución (cli/).

#include "system_prompt.h"

#include "chatbot/config.h"
#include "temp_dir.hpp"

#include <catch2/catch_test_macros.hpp>

#include <sys/stat.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>

namespace {

using chatbot::ErrorKind;
using chatbot_test::ScopedTempDir;
using chatbot::cli::kDefaultSystemPrompt;
using chatbot::cli::kMaxSystemPromptBytes;

std::string read(const std::filesystem::path& path) {
    std::ifstream file{path};
    std::ostringstream content;
    content << file.rdbuf();
    return content.str();
}

void write(const std::filesystem::path& path, const std::string& content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream{path} << content;
    ::chmod(path.c_str(), 0600);
}

std::filesystem::path config_in(const ScopedTempDir& dir) {
    return dir.path() / "chatbot" / "config.json";
}

} // namespace

TEST_CASE("load_system_prompt_value: llave ausente, vacía y con texto",
          "[sistema][config]") {
    const ScopedTempDir dir;
    const std::filesystem::path config = config_in(dir);

    write(config, R"({"model": "m"})");
    const auto missing = chatbot::load_system_prompt_value(config.string());
    REQUIRE(missing.is_ok());
    CHECK_FALSE(missing.value().has_value());

    write(config, R"({"system_prompt": ""})");
    const auto empty = chatbot::load_system_prompt_value(config.string());
    REQUIRE(empty.is_ok());
    REQUIRE(empty.value().has_value());
    CHECK(empty.value()->empty());

    write(config, "{\"system_prompt\": \"Línea uno\\nAcción: sé breve\\tsí\"}");
    const auto text = chatbot::load_system_prompt_value(config.string());
    REQUIRE(text.is_ok());
    REQUIRE(text.value().has_value());
    CHECK(*text.value() == "Línea uno\nAcción: sé breve\tsí");
}

TEST_CASE("load_system_prompt_value: llave que no es cadena da error Config",
          "[sistema][config]") {
    const ScopedTempDir dir;
    const std::filesystem::path config = config_in(dir);
    for (const char* value : {"42", "null", "[\"a\"]", "{\"a\": 1}", "true"}) {
        write(config, std::string{R"({"system_prompt": )"} + value + "}");
        const auto loaded = chatbot::load_system_prompt_value(config.string());
        REQUIRE(loaded.is_error());
        CHECK(loaded.error().kind == ErrorKind::Config);
        CHECK(loaded.error().message.find("\"system_prompt\"") != std::string::npos);
    }
    // load_config no la lee: una llave rara no impide arrancar.
    write(config, R"({"system_prompt": 42, "model": "m", "base_url": "http://localhost:1/v1"})");
    const auto loaded = chatbot::load_config(chatbot::ConfigOptions{
        [](std::string_view) { return std::string_view{}; },
        [config] { return std::optional<std::string>{config.string()}; }, {}});
    CHECK(loaded.is_ok());
}

TEST_CASE("load_system_prompt_value: archivo inexistente y JSON inválido",
          "[sistema][config]") {
    const ScopedTempDir dir;
    const std::filesystem::path config = config_in(dir);

    const auto missing = chatbot::load_system_prompt_value(config.string());
    REQUIRE(missing.is_ok());
    CHECK_FALSE(missing.value().has_value());
    CHECK_FALSE(std::filesystem::exists(config));

    for (const char* content : {"{roto", "[1, 2]", "\"cadena\""}) {
        write(config, content);
        const auto invalid = chatbot::load_system_prompt_value(config.string());
        REQUIRE(invalid.is_error());
        CHECK(invalid.error().kind == ErrorKind::Config);
        // Guardar no toca un archivo inválido.
        const auto error = chatbot::save_system_prompt(config.string(), std::string{"x"});
        REQUIRE(error.has_value());
        CHECK(error->kind == ErrorKind::Config);
        CHECK(read(config) == content);
        CHECK(chatbot::save_system_prompt(config.string(), std::nullopt).has_value());
        CHECK(read(config) == content);
    }
}

TEST_CASE("save_system_prompt conserva las demás llaves y su orden", "[sistema][config]") {
    const ScopedTempDir dir;
    const std::filesystem::path config = config_in(dir);

    // Sin archivo: se crea.
    REQUIRE_FALSE(chatbot::save_system_prompt(config.string(), std::string{"Hola"}).has_value());
    CHECK(chatbot::load_system_prompt_value(config.string()).value() == "Hola");

    write(config,
          R"({"provider": "nvidia", "appearance": {"theme": "catppuccin-mocha", "background": "terminal"}, "system_prompt": "viejo", "zeta": [1, 2]})");
    const std::string prompt = "Responde solo con la palabra PIÑA\nsin acentos raros: ñ á";
    REQUIRE_FALSE(chatbot::save_system_prompt(config.string(), prompt).has_value());
    const std::string text = read(config);
    CHECK(text.find("\"provider\"") < text.find("\"appearance\""));
    CHECK(text.find("\"appearance\"") < text.find("\"system_prompt\""));
    CHECK(text.find("\"system_prompt\"") < text.find("\"zeta\""));
    CHECK(text.find("\"viejo\"") == std::string::npos);
    CHECK(chatbot::load_system_prompt_value(config.string()).value() == prompt);
    const auto appearance = chatbot::load_appearance_values(config.string());
    REQUIRE(appearance.is_ok());
    CHECK(appearance.value().theme == "catppuccin-mocha");
    CHECK(appearance.value().background == "terminal");

    // Cadena vacía: se guarda como tal (sin mensaje de sistema).
    REQUIRE_FALSE(chatbot::save_system_prompt(config.string(), std::string{}).has_value());
    CHECK(chatbot::load_system_prompt_value(config.string()).value() == std::string{});

    // Las otras funciones de guardado no tocan la llave.
    REQUIRE_FALSE(chatbot::save_appearance(config.string(), {"terminal", "theme"}).has_value());
    REQUIRE_FALSE(chatbot::save_config_file(config.string(),
                                            {"openai", "https://api.openai.com/v1", "x"})
                      .has_value());
    CHECK(chatbot::load_system_prompt_value(config.string()).value() == std::string{});
}

TEST_CASE("save_system_prompt con nullopt borra la llave", "[sistema][config]") {
    const ScopedTempDir dir;
    const std::filesystem::path config = config_in(dir);
    write(config, R"({"model": "m", "system_prompt": "x", "zeta": 3})");
    REQUIRE_FALSE(chatbot::save_system_prompt(config.string(), std::nullopt).has_value());
    const std::string text = read(config);
    CHECK(text.find("system_prompt") == std::string::npos);
    CHECK(text.find("\"model\"") < text.find("\"zeta\""));
    CHECK_FALSE(chatbot::load_system_prompt_value(config.string()).value().has_value());
    // Sin la llave, borrarla otra vez no es error.
    CHECK_FALSE(chatbot::save_system_prompt(config.string(), std::nullopt).has_value());
}

TEST_CASE("resolve_system_prompt: predeterminadas, cadena y aviso", "[sistema]") {
    using chatbot::cli::resolve_system_prompt;
    using Loaded = chatbot::Result<std::optional<std::string>>;

    const auto missing = resolve_system_prompt(Loaded{std::optional<std::string>{}});
    CHECK(missing.text == kDefaultSystemPrompt);
    CHECK(missing.warning.empty());

    const auto empty = resolve_system_prompt(Loaded{std::optional<std::string>{""}});
    CHECK(empty.text.empty());
    CHECK(empty.warning.empty());

    const auto custom = resolve_system_prompt(Loaded{std::optional<std::string>{"Sé breve."}});
    CHECK(custom.text == "Sé breve.");
    CHECK(custom.warning.empty());

    const auto wrong = resolve_system_prompt(Loaded{chatbot::ChatError{
        ErrorKind::Config, 0, "\"system_prompt\" de x debe ser una cadena.", std::nullopt}});
    CHECK(wrong.text == kDefaultSystemPrompt);
    CHECK(wrong.warning.find("debe ser una cadena") != std::string::npos);
    CHECK(wrong.warning.find("predeterminadas") != std::string::npos);
}

TEST_CASE("validate_system_prompt: límite de bytes", "[sistema]") {
    using chatbot::cli::validate_system_prompt;
    CHECK_FALSE(validate_system_prompt("").has_value());
    CHECK_FALSE(validate_system_prompt(std::string(kMaxSystemPromptBytes, 'a')).has_value());
    const auto over = validate_system_prompt(std::string(kMaxSystemPromptBytes + 1, 'a'));
    REQUIRE(over.has_value());
    CHECK(over->find("8001") != std::string::npos);
    CHECK(over->find("8000") != std::string::npos);
    // Se cuentan bytes, no caracteres: "ñ" son dos.
    std::string accents;
    while (accents.size() < kMaxSystemPromptBytes) {
        accents += "ñ";
    }
    CHECK_FALSE(validate_system_prompt(accents).has_value());
    CHECK(validate_system_prompt(accents + "a").has_value());
}

TEST_CASE("validate_system_prompt: UTF-8 inválido", "[sistema]") {
    using chatbot::cli::validate_system_prompt;
    for (const std::string& bad : {std::string{"a\xFF"}, std::string{"\xC3"},
                                   std::string{"\xC0\xAF"},        // Sobrelarga.
                                   std::string{"\xED\xA0\x80"},    // Sustituto.
                                   std::string{"\xF4\x90\x80\x80"}, // Mayor que U+10FFFF.
                                   std::string{"\xE2\x82"}}) {
        const auto error = validate_system_prompt(bad);
        REQUIRE(error.has_value());
        CHECK(error->find("UTF-8") != std::string::npos);
    }
    CHECK_FALSE(validate_system_prompt("ñ € 𝄞 中").has_value());
}

TEST_CASE("validate_system_prompt: controles", "[sistema]") {
    using chatbot::cli::validate_system_prompt;
    CHECK_FALSE(validate_system_prompt("uno\ndos\tcon tabulador\n").has_value());
    for (const std::string& bad :
         {std::string{"a\x1b[31m"}, std::string{"a\rb"}, std::string{"a\x7f"},
          std::string{"a\xC2\x85"}, std::string{"a\xC2\x9F"}, std::string{"\x01"},
          std::string{"x", 1} + std::string{"\0", 1}}) {
        const auto error = validate_system_prompt(bad);
        REQUIRE(error.has_value());
        CHECK(error->find("control") != std::string::npos);
    }
    const auto line = validate_system_prompt("uno\ndos\nt\x07res");
    REQUIRE(line.has_value());
    CHECK(line->find("línea 3") != std::string::npos);
    // U+00A0 (después de C1) sí se acepta.
    CHECK_FALSE(validate_system_prompt("a\xC2\xA0" "b").has_value());
}

TEST_CASE("system_prompt_limit_warning contra history_limit_bytes", "[sistema]") {
    using chatbot::cli::system_prompt_limit_warning;
    CHECK_FALSE(system_prompt_limit_warning(std::string(99, 'a'), 100).has_value());
    const auto equal = system_prompt_limit_warning(std::string(100, 'a'), 100);
    REQUIRE(equal.has_value());
    CHECK(equal->find("100") != std::string::npos);
    CHECK(system_prompt_limit_warning(std::string(500, 'a'), 100).has_value());
    // Sin límite (0) no hay aviso.
    CHECK_FALSE(system_prompt_limit_warning(std::string(500, 'a'), 0).has_value());
    CHECK_FALSE(system_prompt_limit_warning("", 0).has_value());
}

TEST_CASE("system_prompt_to_store: el predeterminado borra la llave", "[sistema]") {
    using chatbot::cli::system_prompt_to_store;
    CHECK_FALSE(system_prompt_to_store(kDefaultSystemPrompt).has_value());
    CHECK(system_prompt_to_store("") == std::string{});
    CHECK(system_prompt_to_store("Otro") == std::string{"Otro"});
}
