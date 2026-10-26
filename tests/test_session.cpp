// Pruebas de Session: la lógica de la app sin FTXUI, sin terminal ni red.
// El post mete las tareas en una cola que la prueba vacía, como en
// test_request_runner.cpp.

#include "session.h"

#include "command_parser.h"
#include "downloads.h"
#include "search_settings.h"

#include "blocking_transport.hpp"
#include "fake_search_provider.hpp"
#include "fake_transport.hpp"
#include "performance.hpp"
#include "system_prompt_invariants.hpp"
#include "temp_dir.hpp"

#include "chatbot/config.h"
#include "chatbot/credentials.h"
#include "chatbot/platform.h"
#include "chatbot/search_context.h"
#include "chatbot/system_prompt.h"
#include "chatbot/tavily_search.h"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

using chatbot::cli::Session;
using chatbot::cli::SubmitOutcome;
using chatbot_test::BlockingTransport;
using chatbot_test::kThreadTimeout;
using Type = SubmitOutcome::Type;

/// Fecha fija de /buscar y /exportar.
constexpr std::time_t kNow = 1767225600; // 2026-01-01 UTC.
constexpr const char* kSearchKey = "tvly-clave-ficticia-de-busqueda";

/// Flujo SSE con un delta por pedazo (el texto ya escapado para JSON).
chatbot::HttpResponse stream(const std::string& piece) {
    chatbot::HttpResponse response;
    response.status = 200;
    response.body =
        "data: {\"choices\":[{\"delta\":{\"content\":\"" + piece + "\"}}]}\n\ndata: [DONE]\n\n";
    return response;
}

/// Como stream, con choices[0].finish_reason en el último evento.
chatbot::HttpResponse stream_with_finish(const std::string& piece, const std::string& reason) {
    chatbot::HttpResponse response;
    response.status = 200;
    response.body = "data: {\"choices\":[{\"delta\":{\"content\":\"" + piece +
                    "\"},\"finish_reason\":\"" + reason + "\"}]}\n\ndata: [DONE]\n\n";
    return response;
}

/// La app con un directorio temporal para config.json, credentials.json,
/// las conversaciones y las descargas.
struct Harness {
    chatbot_test::ScopedTempDir dir;
    chatbot_test::TaskQueue queue;
    chatbot_test::FakeEnv env;
    /// Respuestas del modelo de cada cliente nuevo (FakeTransport).
    std::vector<chatbot::HttpResponse> responses{stream("Hola, soy el asistente.")};
    /// Si está, el transporte de cada cliente nuevo en lugar del FakeTransport.
    std::function<std::unique_ptr<chatbot::Transport>()> chat_transport;
    chatbot_test::FakeTransport* chat = nullptr; ///< El del último cliente.
    int clients = 0;
    std::vector<chatbot::HttpResponse> search_responses{chatbot_test::tavily_response(
        nlohmann::json::array({{{"title", "T"}, {"url", "https://ejemplo.mx"}, {"content", "c"}}}))};
    std::optional<std::string> download_base;
    std::vector<Session::Finished> finished; ///< Cada on_finished.
    std::unique_ptr<Session> session; ///< Al último: se destruye primero (join).

    [[nodiscard]] std::string path(const char* name) const { return (dir.path() / name).string(); }
    [[nodiscard]] std::string config_path() const { return path("config.json"); }
    [[nodiscard]] std::string credentials_path() const { return path("credentials.json"); }

    /// Como main: load_config con allow_missing_key_and_model.
    [[nodiscard]] chatbot::Config load() const {
        chatbot::ConfigOptions options;
        options.env = env;
        options.path = [this] { return std::optional<std::string>{config_path()}; };
        options.credentials_path = [this] {
            return std::optional<std::string>{credentials_path()};
        };
        options.allow_missing_key_and_model = true;
        const chatbot::Result<chatbot::Config> config = chatbot::load_config(options);
        REQUIRE(config.is_ok());
        return config.value();
    }

    void start(chatbot::Config config = chatbot_test::test_config()) {
        chatbot::cli::SessionEnvironment environment;
        environment.config_path = config_path();
        environment.credentials_path = credentials_path();
        environment.data_dir = path("conversations");
        environment.env = [this](std::string_view name) { return env(name); };
        environment.make_client = [this](const chatbot::Config& complete) {
            ++clients;
            std::unique_ptr<chatbot::Transport> transport;
            if (chat_transport) {
                transport = chat_transport();
            } else {
                auto fake = std::make_unique<chatbot_test::FakeTransport>();
                fake->responses = responses;
                chat = fake.get();
                transport = std::move(fake);
            }
            return std::make_unique<chatbot::ChatClient>(
                complete, std::move(transport), std::make_unique<chatbot_test::FakeSleeper>());
        };
        environment.make_search = [this](const std::string& key) {
            auto fake = std::make_unique<chatbot_test::FakeTransport>();
            fake->responses = search_responses;
            return std::make_shared<chatbot::TavilySearch>(key, std::move(fake));
        };
        environment.now = [] { return kNow; };
        environment.download_base = [this] { return download_base; };
        session = std::make_unique<Session>(
            std::move(environment), std::move(config),
            [this](chatbot::cli::RequestRunner::Task task) { queue.post(std::move(task)); },
            [this](Session::Finished done) { finished.push_back(std::move(done)); });
    }

    /// Vacía la cola hasta el siguiente on_finished.
    bool finish() {
        const std::size_t before = finished.size();
        return queue.run_until([&] { return finished.size() > before; });
    }

    /// Envía text y espera la respuesta; true si se guardó el par.
    bool exchange(const std::string& text) {
        REQUIRE(session->submit(text).type == Type::Started);
        REQUIRE(finish());
        return finished.back().saved;
    }

    /// Cada cliente nuevo usa un BlockingTransport que se queda esperando
    /// después de un delta; blocking apunta al último.
    void block(BlockingTransport*& blocking) {
        chat_transport = [&blocking] {
            auto transport = std::make_unique<BlockingTransport>(BlockingTransport::Mode::Block);
            transport->first_chunk = "data: {\"choices\":[{\"delta\":{\"content\":\"parcial\"}}]}\n\n";
            blocking = transport.get();
            return transport;
        };
    }

    [[nodiscard]] const chatbot::cli::Entry& last_entry() const {
        REQUIRE_FALSE(session->conversation().entries().empty());
        return session->conversation().entries().back();
    }
};

void check_status(const SubmitOutcome& got, const std::string& text) {
    CHECK(got.type == Type::Status);
    CHECK(got.status == text);
    CHECK_FALSE(got.clear);
}

} // namespace

TEST_CASE("Session sin configuración no envía, pero los comandos locales responden",
          "[session]") {
    Harness h;
    chatbot::Config config = chatbot_test::test_config();
    config.model.clear();
    h.start(config);
    CHECK_FALSE(h.session->configured());
    CHECK(h.session->model().empty());
    CHECK(h.clients == 0);
    check_status(h.session->submit("hola"), std::string{chatbot::cli::kSetupNotice});
    check_status(h.session->submit("/buscar clima"), std::string{chatbot::cli::kSetupNotice});
    check_status(h.session->submit("/copiar"),
                 chatbot::cli::choose_code_block({}, std::nullopt).problem);
    check_status(h.session->submit("/exportar"), "Todavía no hay respuestas que exportar.");
    CHECK(h.session->conversation().entries().empty());
}

TEST_CASE("Session envía, guarda el par y avisa al terminar", "[session]") {
    Harness h;
    h.start();
    REQUIRE(h.session->configured());
    CHECK(h.session->model() == "modelo-de-prueba");
    const SubmitOutcome sent = h.session->submit("  hola  ");
    CHECK(sent.type == Type::Started);
    CHECK(sent.clear);
    CHECK(h.session->busy());
    REQUIRE(h.finish());
    REQUIRE(h.finished.size() == 1);
    CHECK(h.finished[0].restored == std::nullopt);
    CHECK(h.finished[0].error == std::nullopt);
    CHECK(h.finished[0].saved);
    CHECK(h.finished[0].dropped == 0);
    CHECK_FALSE(h.session->busy());
    const std::string& id = h.session->conversation().id();
    REQUIRE_FALSE(id.empty());
    CHECK(std::filesystem::exists(h.dir.path() / "conversations" / (id + ".json")));
    CHECK(h.last_entry().text == "Hola, soy el asistente.");
    CHECK(chatbot_test::check_invariants(chatbot_test::sent_messages_of(h.chat->requests.back()),
                                         h.session->system_prompt(), chatbot_test::Ending::Sent,
                                         "hola") == "");
}

TEST_CASE("Session regresa el texto tras un error o una cancelación, sin guardar", "[session]") {
    Harness h;
    SECTION("429") {
        chatbot::HttpResponse limited;
        limited.status = 429;
        limited.body = R"({"error":{"message":"demasiadas"}})";
        h.responses = {limited};
        h.start();
        CHECK_FALSE(h.exchange("hola"));
        CHECK(h.finished.back().restored == "hola");
        REQUIRE(h.finished.back().error.has_value());
        CHECK(h.finished.back().error->kind == chatbot::ErrorKind::RateLimited);
        CHECK(h.last_entry().kind == chatbot::cli::EntryKind::Error);
    }
    SECTION("cancelación a la mitad") {
        BlockingTransport* blocking = nullptr;
        h.block(blocking);
        h.start();
        REQUIRE(h.session->submit("hola").type == Type::Started);
        REQUIRE(blocking->started.wait_for(kThreadTimeout));
        h.session->cancel();
        REQUIRE(h.finish());
        CHECK(h.finished.back().restored == "hola");
        REQUIRE(h.finished.back().error.has_value());
        CHECK(h.finished.back().error->kind == chatbot::ErrorKind::Cancelled);
        CHECK(h.last_entry().kind == chatbot::cli::EntryKind::Assistant);
        CHECK(h.last_entry().cancelled);
    }
    CHECK(h.session->conversation().id().empty());
    CHECK(h.session->store().list().empty());
    CHECK_FALSE(h.session->busy());
}

TEST_CASE("Session: /buscar", "[session]") {
    Harness h;
    SECTION("sin consulta") {
        h.start();
        check_status(h.session->submit("/buscar"), std::string{chatbot::cli::kSearchUsage});
    }
    SECTION("sin key") {
        h.start();
        CHECK_FALSE(h.session->command_context().has_search_key);
        check_status(h.session->submit("/buscar clima"),
                     std::string{chatbot::cli::kMissingSearchKey});
    }
    SECTION("con resultados: buscando hasta que llega la búsqueda, y se guarda con el par") {
        h.env.values["CHAT_SEARCH_API_KEY"] = kSearchKey;
        h.start();
        REQUIRE(h.session->submit("/buscar clima").type == Type::Started);
        CHECK(h.session->searching());
        REQUIRE(h.queue.run_until([&] { return !h.session->searching(); }));
        CHECK(h.session->busy()); // Se apagó al llegar la búsqueda, no al terminar.
        REQUIRE(h.finish());
        REQUIRE(h.finished.back().restored == std::nullopt);
        CHECK_FALSE(h.session->searching());
        const chatbot::cli::LoadResult loaded =
            h.session->store().load(h.session->conversation().id());
        REQUIRE(loaded.conversation.has_value());
        const auto& search = loaded.conversation->messages.front().search;
        REQUIRE(search.has_value());
        CHECK(search->date == chatbot::local_iso_date(kNow));
        CHECK(search->response.results.size() == 1);
    }
    SECTION("sin resultados: regresa el texto sin llamar al modelo") {
        h.env.values["CHAT_SEARCH_API_KEY"] = kSearchKey;
        h.search_responses = {chatbot_test::tavily_response(nlohmann::json::array())};
        h.start();
        CHECK_FALSE(h.exchange("/buscar nada"));
        CHECK(h.finished.back().restored == "/buscar nada");
        REQUIRE(h.finished.back().error.has_value());
        CHECK(h.finished.back().error->kind == chatbot::ErrorKind::NoSearchResults);
        CHECK(h.chat->requests.empty());
        CHECK_FALSE(h.session->searching());
    }
}

TEST_CASE("Session con una respuesta en curso", "[session]") {
    Harness h;
    BlockingTransport* blocking = nullptr;
    h.block(blocking);
    h.start();
    REQUIRE(h.session->submit("hola").type == Type::Started);
    REQUIRE(blocking->started.wait_for(kThreadTimeout));
    CHECK(h.session->submit("otra").type == Type::Ignored);
    check_status(h.session->submit("/copiar"), "Espera a que termine la respuesta");
    check_status(h.session->submit("/exportar"), "Espera a que termine la respuesta");
    const std::optional<std::string> wait{std::string{chatbot::cli::kWaitOrCancel}};
    CHECK(h.session->new_conversation() == wait);
    CHECK(h.session->open("20260101-000000-abcdef") == wait);
    CHECK(h.session->remove("20260101-000000-abcdef") == wait);
    CHECK(h.session->command_context().busy);
    h.session->cancel();
    REQUIRE(h.finish());
}

TEST_CASE("Session abre conversaciones guardadas", "[session]") {
    Harness h;
    chatbot::Config first = chatbot_test::test_config();
    first.model = "modelo-a";
    h.start(first);
    REQUIRE(h.exchange("hola"));
    const std::string id = h.session->conversation().id();
    chatbot::Config second = chatbot_test::test_config();
    second.model = "modelo-b";
    h.start(second);
    REQUIRE(h.session->open(id) == std::nullopt);
    CHECK(h.session->conversation().id() == id);
    CHECK(h.last_entry().text == "Esta conversación usó modelo-a; se continúa con modelo-b.");

    // Ilegible: el error de store.load, y la abierta no cambia.
    std::ofstream{h.dir.path() / "conversations" / "20260101-000000-abcdef.json"} << "{no es json";
    const std::optional<std::string> error = h.session->open("20260101-000000-abcdef");
    REQUIRE(error.has_value());
    CHECK(*error == h.session->store().load("20260101-000000-abcdef").error);
    CHECK(h.session->conversation().id() == id);
}

TEST_CASE("Session borra conversaciones", "[session]") {
    Harness h;
    h.start();
    REQUIRE(h.exchange("uno"));
    const std::string other = h.session->conversation().id();
    REQUIRE(h.session->new_conversation() == std::nullopt);
    CHECK(h.session->conversation().entries().empty());
    REQUIRE(h.exchange("dos"));
    const std::string open = h.session->conversation().id();
    REQUIRE(other != open);

    REQUIRE(h.session->remove(other) == std::nullopt);
    CHECK(h.session->conversation().id() == open);
    CHECK(h.session->conversation().has_turns());
    CHECK(h.session->store().list().size() == 1);

    REQUIRE(h.session->remove(open) == std::nullopt);
    CHECK(h.session->conversation().id().empty());
    CHECK(h.session->conversation().entries().empty());
    CHECK(h.session->store().list().empty());
}

TEST_CASE("Session: /copiar, /guardar y /exportar", "[session]") {
    Harness h;
    h.responses = {stream("Mira:\\n\\n```cpp\\nint x;\\n```")};
    h.start();
    check_status(h.session->submit("/exportar"), "Todavía no hay respuestas que exportar.");
    REQUIRE(h.exchange("un bloque"));
    const std::vector<chatbot::cli::CodeBlock>& blocks = h.session->code_blocks().blocks();
    REQUIRE(blocks.size() == 1);
    CHECK(h.session->command_context().has_code_blocks);

    check_status(h.session->submit("/copiar tres"), std::string{chatbot::cli::kCopyUsage});
    check_status(h.session->submit("/guardar suma.cpp"), std::string{chatbot::cli::kSaveUsage});
    check_status(h.session->submit("/exportar todo"), std::string{chatbot::cli::kExportUsage});
    check_status(h.session->submit("/copiar 2"), chatbot::cli::choose_code_block(blocks, 2).problem);
    check_status(h.session->submit("/guardar 1 ../x"),
                 *chatbot::cli::validate_file_name("../x"));
    const std::string no_dir{chatbot::cli::no_download_dir_message(chatbot::current_os())};
    check_status(h.session->submit("/guardar 1"), no_dir);
    check_status(h.session->submit("/exportar"), no_dir);

    const SubmitOutcome copy = h.session->submit("/copiar");
    CHECK(copy.type == Type::Copy);
    CHECK(copy.clear);
    CHECK(copy.block.number == 1);
    CHECK(copy.block.code == blocks.front().code);
    h.session->report({"copiado", false});
    CHECK(h.last_entry().kind == chatbot::cli::EntryKind::Notice);
    h.session->report({"no se pudo", true});
    CHECK(h.last_entry().kind == chatbot::cli::EntryKind::Error);

    h.download_base = h.path("descargas");
    std::filesystem::create_directory(*h.download_base);
    const SubmitOutcome saved = h.session->submit("/guardar 1 suma.cpp");
    CHECK(saved.type == Type::Done);
    CHECK(saved.clear);
    CHECK(h.last_entry().kind == chatbot::cli::EntryKind::Notice);
    CHECK(std::filesystem::exists(h.dir.path() / "descargas" / "chatbot" / "suma.cpp"));

    // Error de escritura: la carpeta queda debajo de un archivo normal (igual
    // en POSIX y en Windows). El error va al historial y la caja no se vacía.
    std::ofstream{h.dir.path() / "archivo"} << "x";
    h.download_base = (h.dir.path() / "archivo" / "sub").string();
    const SubmitOutcome failed = h.session->submit("/guardar 1 otro.cpp");
    CHECK(failed.type == Type::Done);
    CHECK_FALSE(failed.clear);
    CHECK(h.last_entry().kind == chatbot::cli::EntryKind::Error);
    h.download_base = h.path("descargas");

    const SubmitOutcome exported = h.session->submit("/exportar");
    CHECK(exported.type == Type::Done);
    CHECK(exported.clear);
    CHECK(std::filesystem::exists(h.dir.path() / "descargas" / "chatbot" /
                                  ("conversacion-" + h.session->conversation().id() + ".md")));
}

TEST_CASE("Session guarda el proveedor y reconstruye el cliente", "[session]") {
    Harness h;
    chatbot::Credentials credentials;
    credentials.keys["nvidia"] = "sk-clave-secreta-de-prueba-1234";
    REQUIRE_FALSE(chatbot::save_credentials(h.credentials_path(), credentials).has_value());
    REQUIRE_FALSE(chatbot::save_config_file(h.config_path(),
                                            {"nvidia", "https://integrate.api.nvidia.com/v1", "m1"})
                      .has_value());
    SECTION("otro modelo") {
        h.start(h.load());
        REQUIRE(h.exchange("hola"));
        const std::string id = h.session->conversation().id();
        chatbot::Result<chatbot::cli::ProviderSettings> form = h.session->provider_form();
        REQUIRE(form.is_ok());
        chatbot::cli::ProviderSettings changed = form.value();
        REQUIRE(changed.set_model("m2"));
        REQUIRE(h.session->save_provider(changed) == std::nullopt);
        CHECK(h.clients == 2);
        CHECK(h.session->model() == "m2");
        CHECK(h.session->config().model == "m2");
        CHECK(h.last_entry().text == "Ahora se usa m2 de " + changed.provider_label() + ".");
        CHECK(h.session->conversation().id() == id);
        CHECK(h.session->conversation().has_turns());
    }
    SECTION("con CHAT_MODEL se conserva el modelo guardado") {
        h.env.values["CHAT_MODEL"] = "modelo-del-entorno";
        h.start(h.load());
        chatbot::cli::ProviderSettings form = h.session->provider_form().value();
        CHECK_FALSE(form.set_model("m2"));
        REQUIRE(h.session->save_provider(form) == std::nullopt);
        CHECK(h.session->model() == "modelo-del-entorno");
        CHECK(chatbot::load_config_file_values(h.config_path()).value().model == "m1");
    }
}

TEST_CASE("Session guarda las instrucciones del sistema", "[session]") {
    Harness h;
    chatbot::Config config = chatbot_test::test_config();
    config.history_limit_bytes = 50;
    h.start(config);
    CHECK(h.session->system_prompt() == chatbot::kDefaultSystemPrompt);

    REQUIRE(h.session->save_system_prompt(std::string{}) == std::nullopt);
    CHECK(h.last_entry().text == "Sin instrucciones del sistema desde el siguiente mensaje.");
    REQUIRE(h.exchange("uno"));
    CHECK(chatbot_test::check_invariants(chatbot_test::sent_messages_of(h.chat->requests.back()),
                                         "", chatbot_test::Ending::Sent, "uno") == "");

    const std::string long_prompt(80, 'x');
    REQUIRE(h.session->save_system_prompt(long_prompt) == std::nullopt);
    const std::vector<chatbot::cli::Entry>& entries = h.session->conversation().entries();
    REQUIRE(entries.size() >= 2);
    CHECK(entries[entries.size() - 2].text ==
          "Instrucciones del sistema actualizadas: valen desde el siguiente mensaje.");
    CHECK(entries.back().text == chatbot::system_prompt_limit_warning(long_prompt, 50).value());
    REQUIRE(h.exchange("dos"));
    CHECK(chatbot_test::check_invariants(chatbot_test::sent_messages_of(h.chat->requests.back()),
                                         long_prompt, chatbot_test::Ending::Sent, "dos") == "");

    REQUIRE(h.session->save_system_prompt(std::nullopt) == std::nullopt); // Restaurar.
    CHECK(h.session->system_prompt() == chatbot::kDefaultSystemPrompt);
    // Las predeterminadas también miden más que el límite de 50.
    CHECK(entries[entries.size() - 2].text ==
          "Instrucciones del sistema actualizadas: valen desde el siguiente mensaje.");
    CHECK(entries.back().text ==
          chatbot::system_prompt_limit_warning(chatbot::kDefaultSystemPrompt, 50).value());
    REQUIRE(h.exchange("tres"));
    CHECK(chatbot_test::check_invariants(chatbot_test::sent_messages_of(h.chat->requests.back()),
                                         chatbot::kDefaultSystemPrompt, chatbot_test::Ending::Sent,
                                         "tres") == "");
}

TEST_CASE("Session guarda la key de búsqueda", "[session]") {
    Harness h;
    h.start();
    REQUIRE(h.session->save_search_key(kSearchKey) == std::nullopt);
    CHECK(h.session->command_context().has_search_key);
    CHECK(h.last_entry().text == "Key de búsqueda guardada: usa /buscar <consulta>.");
    CHECK(h.session->submit("/buscar clima").type == Type::Started);
    REQUIRE(h.finish());
}

TEST_CASE("Session: avisos de arranque", "[session]") {
    Harness h;
    std::ofstream{h.config_path()} << R"({"system_prompt": 5})";
    const std::string prompt_warning =
        chatbot::resolve_system_prompt(chatbot::load_system_prompt_value(h.config_path())).warning;
    REQUIRE_FALSE(prompt_warning.empty());
#ifndef _WIN32
    SECTION("con credentials.json de permisos abiertos") {
        chatbot::Credentials credentials;
        credentials.keys[std::string{chatbot::kSearchCredentialsKey}] = kSearchKey;
        REQUIRE_FALSE(chatbot::save_credentials(h.credentials_path(), credentials).has_value());
        std::filesystem::permissions(h.credentials_path(), std::filesystem::perms::others_read,
                                     std::filesystem::perm_options::add);
        const chatbot::Result<std::string> key =
            chatbot::load_search_api_key(h.env, h.credentials_path());
        REQUIRE(key.is_error());
        h.start();
        CHECK(h.session->startup_warning() == prompt_warning + " " + key.error().message);
        CHECK(h.session->startup_warnings() ==
              std::vector<std::string>{prompt_warning, key.error().message});
        CHECK_FALSE(h.session->command_context().has_search_key);
    }
#endif
    SECTION("solo las instrucciones") {
        h.start();
        CHECK(h.session->startup_warning() == prompt_warning);
        CHECK(h.session->startup_warnings() == std::vector<std::string>{prompt_warning});
        CHECK(h.session->system_prompt() == chatbot::kDefaultSystemPrompt);
    }
}

TEST_CASE("Session::send no interpreta comandos y dice por qué no envió", "[session]") {
    using chatbot::cli::SendResult;
    Harness h;
    SECTION("sin configuración") {
        chatbot::Config config = chatbot_test::test_config();
        config.model.clear();
        h.start(config);
        CHECK(h.session->send("hola") == SendResult::NotConfigured);
        CHECK(h.session->send("", std::string{"clima"}) == SendResult::NotConfigured);
    }
    SECTION("texto vacío, consulta vacía y sin key") {
        h.start();
        CHECK(h.session->send("   ") == SendResult::Empty);
        CHECK(h.session->send("/buscar clima", std::string{" "}) == SendResult::Empty);
        CHECK(h.session->send("/buscar clima", std::string{"clima"}) ==
              SendResult::MissingSearchKey);
        CHECK(h.session->conversation().entries().empty());
    }
    SECTION("un comando va al modelo tal cual") {
        h.start();
        REQUIRE(h.session->send("/copiar") == SendResult::Sent);
        CHECK(h.session->send("otra") == SendResult::Busy);
        REQUIRE(h.finish());
        CHECK(h.finished.back().saved);
        CHECK(h.session->conversation().entries().front().text == "/copiar");
    }
    SECTION("con búsqueda: text se muestra y se guarda, search va a la búsqueda") {
        h.env.values["CHAT_SEARCH_API_KEY"] = kSearchKey;
        h.start();
        REQUIRE(h.session->send("¿Qué clima hace?", std::string{"clima"}) == SendResult::Sent);
        CHECK(h.session->searching());
        REQUIRE(h.finish());
        REQUIRE(h.finished.back().saved);
        const chatbot::cli::LoadResult loaded =
            h.session->store().load(h.session->conversation().id());
        REQUIRE(loaded.conversation.has_value());
        const auto& user = loaded.conversation->messages.front();
        CHECK(user.content == "¿Qué clima hace?");
        REQUIRE(user.search.has_value());
        CHECK(user.search->response.query == "clima");
        CHECK(h.last_entry().kind == chatbot::cli::EntryKind::Sources);
    }
}

TEST_CASE("Session::Finished: finish_reason, respuesta sin texto y guardado que falla",
          "[session]") {
    Harness h;
    SECTION("finish_reason") {
        h.responses = {stream_with_finish("cortada", "length")};
        h.start();
        REQUIRE(h.exchange("hola"));
        CHECK(h.finished.back().finish_reason == "length");
    }
    SECTION("sin texto: BadResponse con kEmptyAnswer, sin guardar") {
        chatbot::HttpResponse empty;
        empty.status = 200;
        empty.body = "data: [DONE]\n\n";
        h.responses = {empty};
        h.start();
        CHECK_FALSE(h.exchange("hola"));
        CHECK(h.finished.back().restored == "hola");
        REQUIRE(h.finished.back().error.has_value());
        CHECK(h.finished.back().error->kind == chatbot::ErrorKind::BadResponse);
        CHECK(h.finished.back().error->message == chatbot::cli::kEmptyAnswer);
        CHECK(h.finished.back().finish_reason.empty());
    }
    SECTION("guardado que falla: terminó bien, pero saved es false y hay un error") {
        // La carpeta de conversaciones es un archivo: store.save falla.
        std::ofstream{h.path("conversations")} << "x";
        h.start();
        CHECK_FALSE(h.exchange("hola"));
        CHECK(h.finished.back().restored == std::nullopt);
        CHECK(h.finished.back().error == std::nullopt);
        CHECK(h.last_entry().kind == chatbot::cli::EntryKind::Error);
        CHECK(h.last_entry().text.rfind("No se pudo guardar la conversación: ", 0) == 0);
    }
}

TEST_CASE("Session se destruye en menos de 2 s con una petición bloqueada", "[session]") {
    Harness h;
    BlockingTransport* blocking = nullptr;
    h.block(blocking);
    h.start();
    REQUIRE(h.session->submit("hola").type == Type::Started);
    REQUIRE(blocking->started.wait_for(kThreadTimeout));
    const auto begin = std::chrono::steady_clock::now();
    h.session.reset(); // Cancela y hace join.
    chatbot_test::report_latency("~Session", std::chrono::steady_clock::now() - begin);
    CHECK(h.finished.empty()); // Lo que quedó en la cola nunca se ejecuta.
}
