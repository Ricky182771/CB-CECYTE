#include "conversation_store.h"

#include "temp_dir.hpp"

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

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
        StoredMessage{Role::User, "¿Cuánto es la raíz cuadrada de 64? 🤔 ñandú 日本語", "", "",
                      std::nullopt},
        StoredMessage{Role::Assistant, "La raíz cuadrada de 64 es **8**. ✅",
                      "nvidia/nemotron-3-super-120b-a12b", "stop", std::nullopt},
        StoredMessage{Role::User, "¿Y de 81?", "", "", std::nullopt},
        StoredMessage{Role::Assistant, "Es 9, pero me cor", "otro/modelo", "length",
                      std::nullopt},
    };
    return conversation;
}

#ifndef _WIN32
unsigned permissions_of(const fs::path& path) {
    struct stat info {};
    REQUIRE(::stat(path.c_str(), &info) == 0);
    return static_cast<unsigned>(info.st_mode) & 0777U;
}
#endif

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

TEST_CASE("resolve_windows_data_dir: CHAT_DATA_DIR, AppData\\Local o LOCALAPPDATA",
          "[almacen][ruta][windows]") {
    using chatbot::cli::resolve_windows_data_dir;
    const std::string local = "C:\\Users\\José\\AppData\\Local";
    const std::string expected = local + "\\chatbot\\conversations";
    CHECK(resolve_windows_data_dir(std::string{"D:\\chats"}, local, local) == "D:\\chats");
    CHECK(resolve_windows_data_dir(std::string{""}, local, std::nullopt) == expected);
    CHECK(resolve_windows_data_dir(std::nullopt, local + "\\", std::string{"E:\\x"}) == expected);
    CHECK(resolve_windows_data_dir(std::nullopt, std::nullopt, local) == expected);
    CHECK(resolve_windows_data_dir(std::nullopt, std::string{}, local) == expected);
    CHECK_FALSE(resolve_windows_data_dir(std::nullopt, std::nullopt, std::nullopt).has_value());
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

TEST_CASE("ConversationStore: ida y vuelta en una carpeta con ñ y acentos", "[almacen]") {
    // Como %LOCALAPPDATA% de un usuario "José Ñandú": la ruta es UTF-8.
    const ScopedTempDir temp;
    const fs::path dir = temp.path() / fs::path{u8"José Ñandú"} / fs::path{u8"conversación"};
    const ConversationStore store{dir.string()};
    const StoredConversation original = sample("20261002-235800-a1b2c3", "2026-10-02T23:59:12-06:00");
    REQUIRE_FALSE(store.save(original).has_value());

    const LoadResult loaded = store.load(original.id);
    REQUIRE(loaded.conversation.has_value());
    CHECK(loaded.conversation->title == original.title);
    REQUIRE(loaded.conversation->messages.size() == original.messages.size());
    CHECK(loaded.conversation->messages[0].content == original.messages[0].content);

    const std::vector<ConversationSummary> list = store.list();
    REQUIRE(list.size() == 1);
    CHECK(list[0].id == original.id);
    CHECK(list[0].readable);
    CHECK_FALSE(has_tmp_files(dir));
    CHECK_FALSE(store.remove(original.id).has_value());
    CHECK(store.list().empty());
}

TEST_CASE("ConversationStore: permisos 0700 del directorio y 0600 del archivo, sin .tmp",
          "[almacen]") {
    const ScopedTempDir temp;
    const fs::path dir = temp.path() / "nuevo" / "conversaciones";
    const ConversationStore store{dir.string()};
    REQUIRE_FALSE(store.save(sample("20261002-235800-a1b2c3", "2026-10-02T23:59:12-06:00"))
                      .has_value());

#ifndef _WIN32
    CHECK(permissions_of(dir) == 0700U);
    CHECK(permissions_of(dir / "20261002-235800-a1b2c3.json") == 0600U);
#endif
    CHECK_FALSE(has_tmp_files(dir));

    // Guardar otra vez (sobrescribir) mantiene 0600 y no deja .tmp.
    REQUIRE_FALSE(store.save(sample("20261002-235800-a1b2c3", "2026-10-03T00:10:00-06:00"))
                      .has_value());
#ifndef _WIN32
    CHECK(permissions_of(dir / "20261002-235800-a1b2c3.json") == 0600U);
#endif
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

#ifndef _WIN32
// En Windows, fs::permissions solo cambia el atributo de solo lectura, que
// no impide escribir dentro de una carpeta: la prueba es de POSIX.
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
#endif

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

TEST_CASE("ConversationStore: el JSON sigue el orden del esquema", "[almacen]") {
    const ScopedTempDir temp;
    const ConversationStore store{temp.string()};
    REQUIRE_FALSE(store.save(sample("20261002-235800-a1b2c3", "2026-10-02T23:59:12-06:00"))
                      .has_value());
    const std::string json = read_file(temp.path() / "20261002-235800-a1b2c3.json");
    const std::vector<std::string> keys = {"\"version\"", "\"id\"", "\"title\"", "\"created_at\"",
                                           "\"updated_at\"", "\"messages\""};
    std::size_t previous = 0;
    for (const std::string& key : keys) {
        INFO("llave " << key);
        const std::size_t at = json.find(key);
        REQUIRE(at != std::string::npos);
        CHECK(at >= previous);
        previous = at;
    }
    // Dentro de cada mensaje del asistente: role, content, model, finish_reason.
    const std::size_t role = json.find("\"role\": \"assistant\"");
    REQUIRE(role != std::string::npos);
    const std::size_t content = json.find("\"content\"", role);
    const std::size_t model = json.find("\"model\"", role);
    const std::size_t finish = json.find("\"finish_reason\"", role);
    CHECK(role < content);
    CHECK(content < model);
    CHECK(model < finish);
}

TEST_CASE("ConversationStore: guardar y cargar conserva la búsqueda de un /buscar",
          "[almacen][busqueda]") {
    const ScopedTempDir temp;
    const ConversationStore store{temp.string()};
    StoredConversation original = sample("20261008-120000-abcdef", "2026-10-08T12:00:00-06:00");
    chatbot::cli::StoredSearch search;
    search.date = "2026-10-08";
    search.response.query = "clima en León";
    search.response.results = {
        {"Pronóstico — León", "https://ejemplo.com/clima", "Soleado, 25 °C.\nViento débil.",
         "2026-10-08"},
        {"", "https://ejemplo.com/b", "Sin título", ""},
    };
    original.messages[0].content = "/buscar clima en León";
    original.messages[0].search = search;

    REQUIRE_FALSE(store.save(original).has_value());
    const LoadResult loaded = store.load(original.id);
    REQUIRE(loaded.conversation.has_value());
    const StoredMessage& first = loaded.conversation->messages[0];
    CHECK(first.content == "/buscar clima en León");
    REQUIRE(first.search.has_value());
    CHECK(first.search->date == "2026-10-08");
    CHECK(first.search->response.query == "clima en León");
    REQUIRE(first.search->response.results.size() == 2);
    for (std::size_t i = 0; i < 2; ++i) {
        const chatbot::SearchResult& a = first.search->response.results[i];
        const chatbot::SearchResult& b = search.response.results[i];
        CHECK(a.title == b.title);
        CHECK(a.url == b.url);
        CHECK(a.content == b.content);
        CHECK(a.published_date == b.published_date);
    }
    // Solo el mensaje que la tenía.
    for (std::size_t i = 1; i < loaded.conversation->messages.size(); ++i) {
        CHECK_FALSE(loaded.conversation->messages[i].search.has_value());
    }
    // Sigue siendo la versión 1, con "search" dentro del mensaje.
    const std::string json = read_file(temp.path() / (original.id + ".json"));
    CHECK_THAT(json, Catch::Matchers::ContainsSubstring("\"version\": 1"));
    const std::size_t at = json.find("\"search\"");
    REQUIRE(at != std::string::npos);
    CHECK(json.find("\"query\"", at) < json.find("\"date\"", at));
    CHECK(json.find("\"date\"", at) < json.find("\"results\"", at));
}

TEST_CASE("ConversationStore: un archivo sin \"search\" y con llaves desconocidas carga igual",
          "[almacen][busqueda]") {
    const ScopedTempDir temp;
    const ConversationStore store{temp.string()};
    write_file(temp.path() / "20261001-100000-000001.json",
               R"({"version": 1, "id": "20261001-100000-000001", "title": "Hola",
                   "futura": {"x": 1},
                   "messages": [{"role": "user", "content": "Hola", "otra": true},
                                {"role": "assistant", "content": "Qué tal", "model": "m",
                                 "finish_reason": "stop"}]})");
    const LoadResult loaded = store.load("20261001-100000-000001");
    REQUIRE(loaded.conversation.has_value());
    REQUIRE(loaded.conversation->messages.size() == 2);
    CHECK_FALSE(loaded.conversation->messages[0].search.has_value());
    CHECK(loaded.conversation->messages[0].content == "Hola");
}

TEST_CASE("ConversationStore: un resultado guardado con URL que no es web se descarta",
          "[almacen][busqueda]") {
    const ScopedTempDir temp;
    const ConversationStore store{temp.string()};
    const std::string id = "20261001-100000-000001";
    for (const std::string bad_url : {"javascript:alert(1)", "file:///etc/passwd"}) {
        INFO(bad_url);
        write_file(temp.path() / (id + ".json"),
                   R"({"version": 1, "id": ")" + id +
                       R"(", "messages": [{"role": "user", "content": "/buscar q",
                       "search": {"query": "q", "date": "2026-10-01", "results": [
                         {"title": "Bueno", "url": "https://ejemplo.com/1", "content": "a"},
                         {"title": "Malo", "url": ")" + bad_url + R"(", "content": "b"},
                         {"title": "Mayúsculas", "url": "HTTPS://EJEMPLO.COM/2", "content": "c"}
                       ]}},
                       {"role": "assistant", "content": "r", "model": "m", "finish_reason": "stop"}]})");
        const LoadResult loaded = store.load(id);
        REQUIRE(loaded.conversation.has_value());
        REQUIRE(loaded.conversation->messages[0].search.has_value());
        const auto& results = loaded.conversation->messages[0].search->response.results;
        // HTTPS:// en mayúsculas se acepta (is_web_url no distingue mayúsculas).
        REQUIRE(results.size() == 2);
        CHECK(results[0].url == "https://ejemplo.com/1");
        CHECK(results[1].url == "HTTPS://EJEMPLO.COM/2");
    }
}

TEST_CASE("ConversationStore: una búsqueda mal formada hace ilegible el archivo",
          "[almacen][busqueda]") {
    const ScopedTempDir temp;
    const ConversationStore store{temp.string()};
    const std::string id = "20261001-100000-000001";
    const std::string bad_searches[] = {
        R"("search": "texto")",
        R"("search": {"query": "q"})",
        R"("search": {"query": "q", "results": [1]})",
        R"("search": {"query": 5, "results": []})",
        R"("search": {"query": "q", "results": [{"url": 3}]})",
    };
    for (const std::string& search : bad_searches) {
        INFO(search);
        write_file(temp.path() / (id + ".json"),
                   R"({"version": 1, "id": ")" + id +
                       R"(", "messages": [{"role": "user", "content": "x", )" + search +
                       R"(}, {"role": "assistant", "content": "r"}]})");
        const LoadResult loaded = store.load(id);
        CHECK_FALSE(loaded.conversation.has_value());
        CHECK_FALSE(loaded.error.empty());
        CHECK(loaded.error.find("alterna") == std::string::npos);
        CHECK(loaded.error.find("seguid") == std::string::npos);
    }
}

TEST_CASE("ConversationStore: mensajes que no alternan usuario → asistente hacen ilegible el archivo",
          "[almacen][sistema]") {
    // Antes se cargaban tal cual y from_stored mandaba, por ejemplo, dos User
    // seguidos al modelo.
    const ScopedTempDir temp;
    const ConversationStore store{temp.string()};
    const std::string id = "20261009-100000-000001";
    const std::string user = R"({"role": "user", "content": "Hola"})";
    const std::string assistant =
        R"({"role": "assistant", "content": "Qué tal", "model": "m", "finish_reason": "stop"})";
    struct Case {
        std::string name;
        std::string messages;
        std::string error;
    };
    const Case cases[] = {
        {"dos user seguidos", user + "," + user + "," + assistant,
         "hay dos mensajes del usuario seguidos"},
        {"dos user seguidos en medio",
         user + "," + assistant + "," + user + "," + user + "," + assistant,
         "hay dos mensajes del usuario seguidos"},
        {"dos assistant seguidos", user + "," + assistant + "," + assistant,
         "hay dos respuestas del asistente seguidas"},
        {"termina en user", user + "," + assistant + "," + user,
         "el último mensaje del usuario no tiene respuesta"},
        {"solo un user", user, "el último mensaje del usuario no tiene respuesta"},
        {"empieza en assistant", assistant + "," + user + "," + assistant,
         "el primer mensaje no es del usuario"},
        {"sin mensajes", "", "no tiene mensajes"},
    };
    for (const Case& test : cases) {
        INFO(test.name);
        const std::string original = R"({"version": 1, "id": ")" + id +
                                     R"(", "title": "Hola", "messages": [)" + test.messages +
                                     "]}";
        write_file(temp.path() / (id + ".json"), original);

        const LoadResult loaded = store.load(id);
        CHECK_FALSE(loaded.conversation.has_value());
        CHECK_THAT(loaded.error, Catch::Matchers::ContainsSubstring(test.error));
        const std::vector<ConversationSummary> listed = store.list();
        REQUIRE(listed.size() == 1);
        CHECK_FALSE(listed[0].readable);
        CHECK_THAT(listed[0].error, Catch::Matchers::ContainsSubstring(test.error));
        // Mismo trato que cualquier ilegible: ni se sobrescribe ni se borra.
        CHECK(store.save(sample(id, "2026-10-09T10:00:00-06:00")).has_value());
        CHECK(store.remove(id).has_value());
        CHECK(read_file(temp.path() / (id + ".json")) == original);
    }

    // Pares bien alternados: carga.
    write_file(temp.path() / (id + ".json"),
               R"({"version": 1, "id": ")" + id + R"(", "messages": [)" + user + "," +
                   assistant + "," + user + "," + assistant + "]}");
    const LoadResult good = store.load(id);
    REQUIRE(good.conversation.has_value());
    CHECK(good.conversation->messages.size() == 4);
}
