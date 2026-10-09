#include "code_blocks.h"
#include "conversation.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

namespace {

using chatbot::cli::CodeBlock;
using chatbot::cli::CodeBlockIndex;
using chatbot::cli::collect_code_blocks;
using chatbot::cli::Entry;
using chatbot::cli::EntryKind;

Entry make(EntryKind kind, std::string text, bool in_progress = false) {
    Entry entry;
    entry.kind = kind;
    entry.text = std::move(text);
    entry.in_progress = in_progress;
    return entry;
}

/// Conversación con bloques en varias respuestas, con y sin lenguaje, en
/// listas, citas y alertas, y un mensaje del usuario con un bloque que no
/// cuenta. (En md4c una nota al pie es un solo párrafo: no lleva bloques.)
std::vector<Entry> sample() {
    return {
        make(EntryKind::User, "```cpp\nno cuenta\n```"),
        make(EntryKind::Assistant, "Uno:\n\n```cpp\nint a;\n```\n\nDos:\n\n```\nsin lenguaje\n```"),
        make(EntryKind::Notice, "```\ntampoco cuenta\n```"),
        make(EntryKind::User, "otra"),
        make(EntryKind::Assistant, "Sin código."),
        make(EntryKind::Assistant,
             "- lista\n\n  ```py\n  print(1)\n  ```\n\n> cita\n>\n> ```sh\n> ls\n> ```\n\n"
             "> [!NOTE]\n> Ver:\n>\n> ```js\n> x()\n> ```\n"),
    };
}

} // namespace

TEST_CASE("collect_code_blocks numera los bloques del asistente en orden", "[code_blocks]") {
    const std::vector<CodeBlock> blocks = collect_code_blocks(sample());
    REQUIRE(blocks.size() == 5);
    for (std::size_t i = 0; i < blocks.size(); ++i) {
        CHECK(blocks[i].number == static_cast<int>(i) + 1);
    }
    CHECK(blocks[0].info == "cpp");
    CHECK(blocks[0].code == "int a;\n");
    CHECK(blocks[0].entry == 1);
    // Sin lenguaje.
    CHECK(blocks[1].info.empty());
    CHECK(blocks[1].code == "sin lenguaje\n");
    CHECK(blocks[1].entry == 1);
    // Dentro de una lista y de una cita.
    CHECK(blocks[2].info == "py");
    CHECK(blocks[2].code == "print(1)\n");
    CHECK(blocks[2].entry == 5);
    CHECK(blocks[3].info == "sh");
    CHECK(blocks[3].code == "ls\n");
    // Dentro de una alerta.
    CHECK(blocks[4].info == "js");
    CHECK(blocks[4].code == "x()\n");
}

TEST_CASE("collect_code_blocks: sin respuestas del asistente no hay bloques", "[code_blocks]") {
    CHECK(collect_code_blocks({}).empty());
    CHECK(collect_code_blocks({make(EntryKind::User, "```\nx\n```")}).empty());
}

TEST_CASE("una respuesta en curso no cambia los números anteriores", "[code_blocks]") {
    std::vector<Entry> entries = sample();
    const std::vector<CodeBlock> before = collect_code_blocks(entries);
    entries.push_back(make(EntryKind::User, "más"));
    entries.push_back(make(EntryKind::Assistant, "Va:\n\n```go\nfunc", true));
    const std::vector<CodeBlock> during = collect_code_blocks(entries);
    REQUIRE(during.size() == before.size() + 1);
    for (std::size_t i = 0; i < before.size(); ++i) {
        CHECK(during[i].number == before[i].number);
        CHECK(during[i].code == before[i].code);
        CHECK(during[i].entry == before[i].entry);
    }
    CHECK(during.back().number == 6);
    CHECK(during.back().info == "go");
    entries.back().text += " main() {}\n```\n\n```\notro\n```";
    const std::vector<CodeBlock> after = collect_code_blocks(entries);
    REQUIRE(after.size() == before.size() + 2);
    CHECK(after[5].code == "func main() {}\n");
    CHECK(after[6].number == 7);
}

TEST_CASE("first_code_numbers da el número del primer bloque de cada entrada", "[code_blocks]") {
    const std::vector<Entry> entries = sample();
    const std::vector<int> numbers =
        chatbot::cli::first_code_numbers(collect_code_blocks(entries), entries.size());
    CHECK(numbers == std::vector<int>{1, 1, 3, 3, 3, 3});
}

TEST_CASE("CodeBlockIndex da lo mismo que collect_code_blocks y reusa la caché", "[code_blocks]") {
    std::vector<Entry> entries = sample();
    CodeBlockIndex index;
    const auto same = [&] {
        const std::vector<CodeBlock> expected = collect_code_blocks(entries);
        REQUIRE(index.blocks().size() == expected.size());
        for (std::size_t i = 0; i < expected.size(); ++i) {
            CHECK(index.blocks()[i].number == expected[i].number);
            CHECK(index.blocks()[i].info == expected[i].info);
            CHECK(index.blocks()[i].code == expected[i].code);
            CHECK(index.blocks()[i].entry == expected[i].entry);
        }
        CHECK(index.first_numbers() ==
              chatbot::cli::first_code_numbers(expected, entries.size()));
    };
    index.update(entries);
    same();
    // Una respuesta que llega por partes.
    entries.push_back(make(EntryKind::Assistant, "```c\nint", true));
    index.update(entries);
    same();
    entries.back().text += " x;\n```\n```\ny\n```";
    index.update(entries);
    same();
    // Otra conversación, más corta.
    entries = {make(EntryKind::Assistant, "```rust\nfn main() {}\n```")};
    index.update(entries);
    same();
    entries.clear();
    index.update(entries);
    same();
}
