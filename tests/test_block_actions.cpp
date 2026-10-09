#include "block_actions.h"
#include "code_blocks.h"
#include "conversation.h"
#include "downloads.h"
#include "temp_dir.hpp"

#include <catch2/catch_test_macros.hpp>

#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

// Sin procesos reales: el lanzador y la terminal son falsos, y los archivos
// van a un directorio temporal.

namespace {

namespace fs = std::filesystem;

using chatbot::cli::ActionResult;
using chatbot_test::native_separators;
using chatbot::cli::ClipboardAccess;
using chatbot::cli::ClipboardMethod;
using chatbot::cli::CodeBlock;

CodeBlock block(int number, std::string info, std::string code, bool complete = true) {
    CodeBlock out;
    out.number = number;
    out.info = std::move(info);
    out.code = std::move(code);
    out.complete = complete;
    out.reason = complete ? chatbot::cli::IncompleteReason::None
                          : chatbot::cli::IncompleteReason::Cancelled;
    return out;
}

ClipboardMethod program(std::string name) {
    ClipboardMethod method;
    method.kind = ClipboardMethod::Kind::Program;
    method.argv = {std::move(name)};
    return method;
}

ClipboardMethod osc52(bool fallback) {
    ClipboardMethod method;
    method.kind = ClipboardMethod::Kind::Osc52;
    method.fallback = fallback;
    return method;
}

/// Portapapeles falso: los programas salen con ok; guarda lo que recibió.
struct FakeClipboard {
    bool program_ok = true;
    std::vector<std::string> programs;
    std::string input;
    std::string terminal;

    ClipboardAccess access(std::vector<ClipboardMethod> methods) {
        return ClipboardAccess{
            std::move(methods), false,
            [this](const std::vector<std::string>& argv, std::string_view text) {
                programs.push_back(argv.front());
                input = std::string{text};
                return program_ok;
            },
            [this](std::string_view sequence) {
                terminal += sequence;
                return true;
            }};
    }
};

std::string read_text(const std::string& path) {
    std::ifstream file(path);
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

} // namespace

TEST_CASE("copy_block: con un programa, con OSC 52 y sin método", "[block_actions]") {
    FakeClipboard fake;
    const CodeBlock code = block(3, "cpp", "int a;\nint b;\n");

    ActionResult result = chatbot::cli::copy_block(code, fake.access({program("wl-copy")}));
    CHECK_FALSE(result.error);
    CHECK(result.message == "Copiado el bloque #3 (cpp, 2 líneas) con wl-copy.");
    CHECK(fake.programs == std::vector<std::string>{"wl-copy"});
    CHECK(fake.input == "int a;\nint b;\n");

    fake.program_ok = false;
    result = chatbot::cli::copy_block(code, fake.access({program("wl-copy"), osc52(true)}));
    CHECK_FALSE(result.error);
    CHECK(result.message == "Copiado el bloque #3 (cpp, 2 líneas) con OSC 52; si tu terminal no "
                            "lo soporta, usa /guardar 3.");
    CHECK(fake.terminal.find("\x1b]52;c;") == 0);

    result = chatbot::cli::copy_block(code, fake.access({program("wl-copy")}));
    CHECK(result.error);
    CHECK(result.message == "No se pudo copiar el bloque #3 (cpp, 2 líneas). Usa /guardar 3.");

    const CodeBlock huge = block(4, "", std::string(chatbot::cli::kOsc52MaxBytes + 1, 'x'));
    result = chatbot::cli::copy_block(huge, fake.access({osc52(true)}));
    CHECK(result.error);
    CHECK(result.message.find("pasa de 100 000 bytes") != std::string::npos);
    CHECK(result.message.find("/guardar 4") != std::string::npos);
}

TEST_CASE("copy_block y save_block avisan si el bloque está incompleto", "[block_actions]") {
    FakeClipboard fake;
    const CodeBlock code = block(3, "cpp", "int a;\n", false);
    const ActionResult copied = chatbot::cli::copy_block(code, fake.access({program("wl-copy")}));
    CHECK(copied.message == "Copiado el bloque #3 (cpp, 1 línea) con wl-copy. Ojo: el bloque "
                            "está incompleto (la respuesta se canceló).");

    const chatbot_test::ScopedTempDir dir;
    const ActionResult saved = chatbot::cli::save_block(code, "", dir.string(), "");
    CHECK_FALSE(saved.error);
    CHECK(saved.message ==
          "Guardado en " + native_separators(dir.string() + "/chatbot/bloque-3.cpp") +
              ". Ojo: el bloque está incompleto (la respuesta se canceló).");
}

TEST_CASE("save_block: nombre por defecto, propio, inválido y carpeta imposible",
          "[block_actions]") {
    const chatbot_test::ScopedTempDir dir;
    const CodeBlock code = block(2, "python", "print(1)");

    ActionResult result = chatbot::cli::save_block(code, "", dir.string(), "");
    CHECK_FALSE(result.error);
    CHECK(result.message ==
          "Guardado en " + native_separators(dir.string() + "/chatbot/bloque-2.py"));
    CHECK(read_text(dir.string() + "/chatbot/bloque-2.py") == "print(1)\n");

    // Nunca sobrescribe; home se abrevia con ~ (en Windows main pasa home
    // vacía y la ruta va completa).
    result = chatbot::cli::save_block(code, "", dir.string(), dir.string());
    if (chatbot::current_os() == chatbot::Os::Posix) {
        CHECK(result.message == "Guardado en ~/chatbot/bloque-2-2.py");
    } else {
        CHECK(result.message ==
              "Guardado en " + native_separators(dir.string() + "/chatbot/bloque-2-2.py"));
    }

    result = chatbot::cli::save_block(code, "hola", dir.string(), "");
    CHECK(result.message == "Guardado en " + native_separators(dir.string() + "/chatbot/hola.py"));

    result = chatbot::cli::save_block(code, "../x", dir.string(), "");
    CHECK(result.error);
    CHECK_FALSE(fs::exists(dir.path() / "x.py"));

    std::ofstream(dir.path() / "archivo") << "no soy carpeta";
    result = chatbot::cli::save_block(code, "", (dir.path() / "archivo").string(), "");
    CHECK(result.error);
    CHECK(result.message.find("No se pudo crear") != std::string::npos);
}

TEST_CASE("export_conversation: sin pares es un error; con pares escribe el Markdown",
          "[block_actions]") {
    const chatbot_test::ScopedTempDir dir;
    chatbot::cli::Conversation conversation{"sistema"};
    const std::time_t now = 1767225600; // 2026-01-01 00:00:00 UTC.
    ActionResult result = chatbot::cli::export_conversation(conversation, now, dir.string(), "");
    CHECK(result.error);
    CHECK(result.message == "Todavía no hay respuestas que exportar.");
    CHECK_FALSE(fs::exists(dir.path() / "chatbot"));

    REQUIRE(conversation.submit("hola").has_value());
    conversation.append_delta("¡Hola!");
    REQUIRE_FALSE(conversation.finish_success("stop", "m").has_value());
    conversation.set_identity("20260101-000000-abcdef", "2026-01-01T00:00:00Z");
    result = chatbot::cli::export_conversation(conversation, now, dir.string(), dir.string());
    CHECK_FALSE(result.error);
    if (chatbot::current_os() == chatbot::Os::Posix) {
        CHECK(result.message == "Conversación exportada a ~/chatbot/conversacion-20260101-000000-"
                                "abcdef.md");
    } else {
        CHECK(result.message ==
              "Conversación exportada a " +
                  native_separators(dir.string() +
                                    "/chatbot/conversacion-20260101-000000-abcdef.md"));
    }
    const std::string text =
        read_text(dir.string() + "/chatbot/conversacion-20260101-000000-abcdef.md");
    CHECK(text.find("¡Hola!") != std::string::npos);
    CHECK(text.find("sistema") == std::string::npos);
}

TEST_CASE("run_block_action: copia o guarda el bloque con ese número", "[block_actions]") {
    using chatbot::cli::BlockAction;
    FakeClipboard fake;
    const std::vector<CodeBlock> blocks{block(1, "cpp", "int a;\n"), block(2, "py", "x = 1\n")};
    const chatbot_test::ScopedTempDir dir;

    ActionResult result = chatbot::cli::run_block_action(
        blocks, 2, BlockAction::Copy, fake.access({program("xclip")}), dir.string(), "");
    CHECK_FALSE(result.error);
    CHECK(fake.input == "x = 1\n");
    CHECK(result.message == "#2 copiado con xclip"); // Botones: aviso corto.

    result = chatbot::cli::run_block_action(blocks, 1, BlockAction::Save,
                                            fake.access({program("xclip")}), dir.string(), "");
    CHECK_FALSE(result.error);
    CHECK(read_text(dir.string() + "/chatbot/bloque-1.cpp") == "int a;\n");
    CHECK(fake.programs.size() == 1); // Guardar no copia.

    result = chatbot::cli::run_block_action(blocks, 1, BlockAction::Save,
                                            fake.access({program("xclip")}), std::nullopt, "");
    CHECK(result.error);
    CHECK(result.message == chatbot::cli::no_download_dir_message(chatbot::current_os()));

    result = chatbot::cli::run_block_action(blocks, 3, BlockAction::Copy,
                                            fake.access({program("xclip")}), dir.string(), "");
    CHECK(result.error);
    CHECK(result.message.find("No existe el bloque #3") != std::string::npos);
}

TEST_CASE("run_block_action: un bloque de la respuesta en curso se copia como va",
          "[block_actions]") {
    chatbot::cli::Conversation conversation{""};
    REQUIRE(conversation.submit("dame código").has_value());
    conversation.append_delta("Va:\n\n```cpp\nint a;\nint b");
    REQUIRE(conversation.busy());
    chatbot::cli::CodeBlockIndex index;
    index.update(conversation.entries());
    FakeClipboard fake;
    const ActionResult result = chatbot::cli::run_block_action(
        index.blocks(), 1, chatbot::cli::BlockAction::Copy, fake.access({program("wl-copy")}),
        std::nullopt, "");
    CHECK_FALSE(result.error);
    CHECK(fake.input == "int a;\nint b\n");
    CHECK(result.message == "⚠ #1 incompleto (aún no termina) · copiado con wl-copy");
    // No tocó la conversación.
    CHECK(conversation.entries().size() == 2);
    CHECK(conversation.busy());
}

TEST_CASE("avisos cortos de los botones, caso por caso", "[block_actions]") {
    using chatbot::cli::IncompleteReason;
    using chatbot::cli::Wording;
    FakeClipboard fake;
    const CodeBlock code = block(1, "cpp", "int a;\n");

    CHECK(chatbot::cli::copy_block(code, fake.access({program("wl-copy")}), Wording::Short)
              .message == "#1 copiado con wl-copy");
    // OSC 52 como primer método (por SSH) no es el último recurso.
    CHECK(chatbot::cli::copy_block(code, fake.access({osc52(false)}), Wording::Short).message ==
          "#1 copiado con OSC 52");
    CHECK(chatbot::cli::copy_block(code, fake.access({osc52(true)}), Wording::Short).message ==
          "#1 copiado con OSC 52 · si no pega, usa [Guardar]");

    CodeBlock unfinished = code;
    unfinished.complete = false;
    for (const auto& [reason, text] :
         {std::pair{IncompleteReason::InProgress, "aún no termina"},
          std::pair{IncompleteReason::Cancelled, "se canceló"},
          std::pair{IncompleteReason::Error, "se cortó"}}) {
        INFO(text);
        unfinished.reason = reason;
        CHECK(chatbot::cli::copy_block(unfinished, fake.access({program("wl-copy")}),
                                       Wording::Short)
                  .message == "⚠ #1 incompleto (" + std::string{text} + ") · copiado con wl-copy");
    }

    fake.program_ok = false;
    ActionResult failed =
        chatbot::cli::copy_block(code, fake.access({program("wl-copy")}), Wording::Short);
    CHECK(failed.error);
    CHECK(failed.message == "No se pudo copiar #1 · usa [Guardar]");
    failed = chatbot::cli::copy_block(block(4, "", std::string(chatbot::cli::kOsc52MaxBytes + 1, 'x')),
                                      fake.access({osc52(true)}), Wording::Short);
    CHECK(failed.message == "No se pudo copiar #4 · usa [Guardar]");

    const chatbot_test::ScopedTempDir dir;
    // Con home = dir, "~/..." en POSIX; en Windows no se abrevia (main pasa
    // home vacía) y va la ruta completa.
    const auto shown = [&dir](const std::string& relative) {
        return chatbot::current_os() == chatbot::Os::Posix
                   ? "~/" + relative
                   : native_separators(dir.string() + "/" + relative);
    };
    ActionResult saved = chatbot::cli::save_block(code, "", dir.string(), dir.string(),
                                                  Wording::Short);
    CHECK_FALSE(saved.error);
    CHECK(saved.message == "#1 guardado en " + shown("chatbot/bloque-1.cpp"));
    unfinished.reason = IncompleteReason::Cancelled;
    saved = chatbot::cli::save_block(unfinished, "", dir.string(), dir.string(), Wording::Short);
    CHECK(saved.message ==
          "⚠ #1 incompleto (se canceló) · guardado en " + shown("chatbot/bloque-1-2.cpp"));

    // Los comandos conservan los textos largos.
    CHECK(chatbot::cli::copy_block(code, fake.access({osc52(true)})).message ==
          "Copiado el bloque #1 (cpp, 1 línea) con OSC 52; si tu terminal no lo soporta, usa "
          "/guardar 1.");
}
