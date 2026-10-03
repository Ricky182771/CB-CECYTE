#include "conversation_store.h"

#include "temp_dir.hpp"

#include <sys/stat.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace {

using chatbot::Role;
using chatbot::cli::ConversationStore;
using chatbot::cli::ConversationSummary;
using chatbot::cli::LoadResult;
using chatbot::cli::StoredConversation;
using chatbot::cli::StoredMessage;
using chatbot_test::ScopedTempDir;
namespace fs = std::filesystem;

StoredConversation sample(const std::string& id, const std::string& updated_at) {
    StoredConversation conversation;
    conversation.id = id;
    conversation.title = "¿Cuánto es la raíz cuadrada de 64?";
    conversation.created_at = "2026-10-02T23:58:00-06:00";
    conversation.updated_at = updated_at;
    conversation.messages = {
        StoredMessage{Role::User, "¿Cuánto es la raíz cuadrada de 64? 🤔 ñandú 日本語", "", ""},
        StoredMessage{Role::Assistant, "La raíz cuadrada de 64 es **8**. ✅",
                      "nvidia/nemotron-3-super-120b-a12b", "stop"},
        StoredMessage{Role::User, "¿Y de 81?", "", ""},
        StoredMessage{Role::Assistant, "Es 9, pero me cor", "otro/modelo", "length"},
    };
    return conversation;
}

unsigned permissions_of(const fs::path& path) {
    struct stat info {};
    REQUIRE(::stat(path.c_str(), &info) == 0);
    return static_cast<unsigned>(info.st_mode) & 0777U;
}

void write_file(const fs::path& path, const std::string& content) {
    std::ofstream file{path, std::ios::binary};
    file << content;
}

std::string read_file(const fs::path& path) {
    std::ifstream file{path, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

bool has_tmp_files(const fs::path& dir) {
    for (const fs::directory_entry& entry : fs::directory_iterator{dir}) {
        if (entry.path().extension() == ".tmp") {
            return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE("resolve_data_dir: CHAT_DATA_DIR gana", "[almacen][ruta]") {
    CHECK(chatbot::cli::resolve_data_dir(std::string{"/datos/chat"}, std::string{"/xdg"},
                                         std::string{"/home/ana"}) == "/datos/chat");
}

TEST_CASE("resolve_data_dir: CHAT_DATA_DIR vacío se ignora", "[almacen][ruta]") {
    CHECK(chatbot::cli::resolve_data_dir(std::string{""}, std::string{"/xdg"},
                                         std::string{"/home/ana"}) ==
          "/xdg/chatbot/conversations");
}

TEST_CASE("resolve_data_dir: XDG_DATA_HOME absoluto", "[almacen][ruta]") {
    CHECK(chatbot::cli::resolve_data_dir(std::nullopt, std::string{"/xdg/"},
                                         std::string{"/home/ana"}) ==
          "/xdg/chatbot/conversations");
}

TEST_CASE("resolve_data_dir: XDG_DATA_HOME relativo, vacío o ausente usan HOME",
          "[almacen][ruta]") {
    const std::string expected = "/home/ana/.local/share/chatbot/conversations";
    CHECK(chatbot::cli::resolve_data_dir(std::nullopt, std::string{"relativa/datos"},
                                         std::string{"/home/ana"}) == expected);
    CHECK(chatbot::cli::resolve_data_dir(std::nullopt, std::string{""},
                                         std::string{"/home/ana"}) == expected);
    CHECK(chatbot::cli::resolve_data_dir(std::nullopt, std::nullopt,
                                         std::string{"/home/ana"}) == expected);
}

TEST_CASE("resolve_data_dir: sin nada devuelve nullopt", "[almacen][ruta]") {
    CHECK_FALSE(chatbot::cli::resolve_data_dir(std::nullopt, std::nullopt, std::nullopt)
                    .has_value());
    CHECK_FALSE(chatbot::cli::resolve_data_dir(std::nullopt, std::string{"rel"},
                                               std::string{""})
                    .has_value());
}

TEST_CASE("make_title: colapsa espacios y saltos de línea", "[almacen][titulo]") {
    CHECK(chatbot::cli::make_title("  Hola\n\n  qué\ttal  ") == "Hola qué tal");
}

TEST_CASE("make_title: trunca a 60 puntos de código sin partir UTF-8", "[almacen][titulo]") {
    // 60 puntos de código exactos con acentos y emoji: no se trunca.
    std::string exact;
    for (int i = 0; i < 30; ++i) {
        exact += "á🙂";
    }
    CHECK(chatbot::cli::make_title(exact) == exact);

    // 61: se trunca en el punto de código 60 y se agrega "…".
    const std::string longer = exact + "é";
    CHECK(chatbot::cli::make_title(longer) == exact + "…");

    // Si el corte cae justo antes de un emoji de 4 bytes, no se parte.
    const std::string ascii59(59, 'a');
    CHECK(chatbot::cli::make_title(ascii59 + "🙂🙂") == ascii59 + "🙂…");
}

TEST_CASE("format_iso8601 y format_conversation_id", "[almacen][fechas]") {
    const std::string iso = chatbot::cli::format_iso8601(0);
    REQUIRE(iso.size() == 25); // AAAA-MM-DDTHH:MM:SS±HH:MM
    CHECK(iso[10] == 'T');
    CHECK((iso[19] == '+' || iso[19] == '-'));
    CHECK(iso[22] == ':');

    const std::string id = chatbot::cli::format_conversation_id(0, 0xa1b2c3);
    REQUIRE(id.size() == 22);
    CHECK(id.substr(16) == "a1b2c3");
    CHECK(id[8] == '-');
    CHECK(id[15] == '-');
}

TEST_CASE("ConversationStore: guardar y cargar conserva todo (UTF-8, emoji, model, finish_reason)",
          "[almacen]") {
    const ScopedTempDir temp;
    const ConversationStore store{(temp.path() / "conversaciones").string()};
    const StoredConversation original = sample("20261002-235800-a1b2c3", "2026-10-02T23:59:12-06:00");

    REQUIRE_FALSE(store.save(original).has_value());
    const LoadResult loaded = store.load(original.id);
    REQUIRE(loaded.conversation.has_value());
    const StoredConversation& copy = *loaded.conversation;
    CHECK(copy.id == original.id);
    CHECK(copy.title == original.title);
    CHECK(copy.created_at == original.created_at);
    CHECK(copy.updated_at == original.updated_at);
    REQUIRE(copy.messages.size() == original.messages.size());
    for (std::size_t i = 0; i < copy.messages.size(); ++i) {
        CHECK(copy.messages[i].role == original.messages[i].role);
        CHECK(copy.messages[i].content == original.messages[i].content);
        CHECK(copy.messages[i].model == original.messages[i].model);
        CHECK(copy.messages[i].finish_reason == original.messages[i].finish_reason);
    }
    // El archivo sigue el esquema versión 1.
    const std::string json = read_file(temp.path() / "conversaciones" / (original.id + ".json"));
    CHECK_THAT(json, Catch::Matchers::ContainsSubstring("\"version\": 1"));
    CHECK_THAT(json, Catch::Matchers::ContainsSubstring("\"role\": \"assistant\""));
    CHECK(json.find("system") == std::string::npos);
}

TEST_CASE("ConversationStore: permisos 0700 del directorio y 0600 del archivo, sin .tmp",
          "[almacen]") {
    const ScopedTempDir temp;
    const fs::path dir = temp.path() / "nuevo" / "conversaciones";
    const ConversationStore store{dir.string()};
    REQUIRE_FALSE(store.save(sample("20261002-235800-a1b2c3", "2026-10-02T23:59:12-06:00"))
                      .has_value());

    CHECK(permissions_of(dir) == 0700U);
    CHECK(permissions_of(dir / "20261002-235800-a1b2c3.json") == 0600U);
    CHECK_FALSE(has_tmp_files(dir));

    // Guardar otra vez (sobrescribir) mantiene 0600 y no deja .tmp.
    REQUIRE_FALSE(store.save(sample("20261002-235800-a1b2c3", "2026-10-03T00:10:00-06:00"))
                      .has_value());
    CHECK(permissions_of(dir / "20261002-235800-a1b2c3.json") == 0600U);
    CHECK_FALSE(has_tmp_files(dir));
}

TEST_CASE("ConversationStore: list ordenada por fecha de actualización, la más reciente primero",
          "[almacen]") {
    const ScopedTempDir temp;
    const ConversationStore store{temp.string()};
    REQUIRE_FALSE(store.save(sample("20261001-100000-000001", "2026-10-01T10:00:00-06:00")).has_value());
    REQUIRE_FALSE(store.save(sample("20261003-090000-000003", "2026-10-03T09:00:00-06:00")).has_value());
    // Mismo instante expresado con otro desfase: 2026-10-02T12:00:00Z.
    REQUIRE_FALSE(store.save(sample("20261002-060000-000002", "2026-10-02T12:00:00+00:00")).has_value());
    write_file(temp.path() / "20261009-000000-ffffff.json.tmp", "{basura");

    const std::vector<ConversationSummary> list = store.list();
    REQUIRE(list.size() == 3); // El .tmp se ignora.
    CHECK(list[0].id == "20261003-090000-000003");
    CHECK(list[1].id == "20261002-060000-000002");
    CHECK(list[2].id == "20261001-100000-000001");
    CHECK(list[0].readable);
    CHECK(list[0].title == "¿Cuánto es la raíz cuadrada de 64?");
    CHECK(list[0].message_count == 4);
    CHECK(list[0].last_model == "otro/modelo");
}

TEST_CASE("ConversationStore: archivos ilegibles se listan marcados y no se tocan", "[almacen]") {
    const ScopedTempDir temp;
    const ConversationStore store{temp.string()};
    REQUIRE_FALSE(store.save(sample("20261001-100000-000001", "2026-10-01T10:00:00-06:00")).has_value());
    const fs::path corrupt = temp.path() / "20261002-100000-00000c.json";
    const fs::path future = temp.path() / "20261002-110000-000099.json";
    write_file(corrupt, "{ esto no es json");
    write_file(future, R"({"version": 99, "id": "20261002-110000-000099", "algo": "nuevo"})");

    const std::vector<ConversationSummary> list = store.list();
    REQUIRE(list.size() == 3);
    CHECK(list[0].readable); // Los legibles primero.
    std::size_t unreadable = 0;
    for (const ConversationSummary& item : list) {
        if (!item.readable) {
            ++unreadable;
            CHECK_FALSE(item.error.empty());
        }
    }
    CHECK(unreadable == 2);

    // load, save y remove no los tocan.
    CHECK_FALSE(store.load("20261002-110000-000099").conversation.has_value());
    StoredConversation overwrite = sample("20261002-110000-000099", "2026-10-03T00:00:00-06:00");
    CHECK(store.save(overwrite).has_value());
    CHECK(store.remove("20261002-110000-000099").has_value());
    CHECK(store.remove("20261002-100000-00000c").has_value());
    CHECK(read_file(future) ==
          R"({"version": 99, "id": "20261002-110000-000099", "algo": "nuevo"})");
    CHECK(read_file(corrupt) == "{ esto no es json");
}

TEST_CASE("ConversationStore: remove borra el archivo", "[almacen]") {
    const ScopedTempDir temp;
    const ConversationStore store{temp.string()};
    REQUIRE_FALSE(store.save(sample("20261001-100000-000001", "2026-10-01T10:00:00-06:00")).has_value());
    REQUIRE_FALSE(store.remove("20261001-100000-000001").has_value());
    CHECK(store.list().empty());
    CHECK(store.remove("20261001-100000-000001").has_value()); // Ya no existe.
}

TEST_CASE("ConversationStore: ids con ruta o caracteres raros se rechazan", "[almacen]") {
    const ScopedTempDir temp;
    const ConversationStore store{temp.string()};
    CHECK_FALSE(store.load("../fuera").conversation.has_value());
    CHECK(store.remove("../../etc/passwd").has_value());
    StoredConversation bad = sample("../fuera", "2026-10-01T10:00:00-06:00");
    CHECK(store.save(bad).has_value());
    CHECK_FALSE(fs::exists(temp.path().parent_path() / "fuera.json"));
}

TEST_CASE("ConversationStore: new_id genera ids únicos y con formato", "[almacen]") {
    const ScopedTempDir temp;
    const ConversationStore store{temp.string()};
    std::set<std::string> ids;
    for (int i = 0; i < 200; ++i) {
        const std::string id = store.new_id();
        CHECK(id.size() == 22);
        ids.insert(id);
    }
    CHECK(ids.size() == 200);
}

TEST_CASE("ConversationStore: sin directorio todas las operaciones fallan con mensaje",
          "[almacen]") {
    const ConversationStore store{""};
    CHECK(store.list().empty());
    CHECK(store.save(sample("20261001-100000-000001", "x")).has_value());
    CHECK_FALSE(store.load("20261001-100000-000001").error.empty());
}

TEST_CASE("ConversationStore: directorio sin permiso de escritura da error, no excepción",
          "[almacen]") {
    if (::geteuid() == 0) {
        SKIP("root ignora los permisos de archivo");
    }
    const ScopedTempDir temp;
    fs::permissions(temp.path(), fs::perms::owner_read | fs::perms::owner_exec);
    const ConversationStore store{temp.string()};
    const std::optional<std::string> error =
        store.save(sample("20261001-100000-000001", "2026-10-01T10:00:00-06:00"));
    REQUIRE(error.has_value());
    CHECK_FALSE(error->empty());
}

TEST_CASE("ConversationStore: si la carpeta es un archivo, guardar da error sin excepción",
          "[almacen]") {
    const ScopedTempDir temp;
    const fs::path not_a_dir = temp.path() / "soy_un_archivo";
    write_file(not_a_dir, "x");
    const ConversationStore store{not_a_dir.string()};
    const std::optional<std::string> error =
        store.save(sample("20261001-100000-000001", "2026-10-01T10:00:00-06:00"));
    REQUIRE(error.has_value());
    CHECK_FALSE(error->empty());
    CHECK(read_file(not_a_dir) == "x");
}
