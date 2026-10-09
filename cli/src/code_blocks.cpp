#include "code_blocks.h"

#include "markdown.h"

#include <algorithm>
#include <utility>

namespace chatbot::cli {

namespace {

/// Recorre los bloques en el mismo orden que el Renderer de markdown_view:
/// cada bloque y luego sus hijos. Las celdas de tabla solo tienen texto en
/// línea, así que no hay código dentro de ellas.
void collect(const std::vector<md::Block>& list, std::vector<CodeBlock>& out) {
    for (const md::Block& block : list) {
        if (block.kind == md::Block::Kind::Code) {
            CodeBlock found;
            found.number = static_cast<int>(out.size()) + 1;
            found.info = block.info;
            found.code = block.code;
            out.push_back(std::move(found));
        }
        collect(block.children, out);
    }
}

} // namespace

std::vector<CodeBlock> code_blocks_of(const md::Document& document) {
    std::vector<CodeBlock> blocks;
    collect(document.blocks, blocks);
    collect(document.footnotes, blocks);
    return blocks;
}

std::vector<CodeBlock> collect_code_blocks(const std::vector<Entry>& entries) {
    std::vector<CodeBlock> blocks;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].kind != EntryKind::Assistant) {
            continue;
        }
        for (CodeBlock& block : code_blocks_of(md::parse(entries[i].text))) {
            block.number = static_cast<int>(blocks.size()) + 1;
            block.entry = i;
            blocks.push_back(std::move(block));
        }
    }
    return blocks;
}

std::vector<int> first_code_numbers(const std::vector<CodeBlock>& blocks,
                                    std::size_t entry_count) {
    std::vector<int> numbers(entry_count, 1);
    // Los bloques van en orden de entrada: cada entrada empieza después del
    // último bloque de las anteriores.
    std::size_t next = 0;
    int number = 1;
    for (std::size_t i = 0; i < entry_count; ++i) {
        numbers[i] = number;
        while (next < blocks.size() && blocks[next].entry == i) {
            ++number;
            ++next;
        }
    }
    return numbers;
}

CodeBlockChoice choose_code_block(const std::vector<CodeBlock>& blocks,
                                  std::optional<int> number) {
    CodeBlockChoice choice;
    if (blocks.empty()) {
        choice.problem = "No hay bloques de código en la conversación.";
        return choice;
    }
    if (!number.has_value()) {
        choice.block = &blocks.back();
        return choice;
    }
    if (*number < 1 || *number > static_cast<int>(blocks.size())) {
        choice.problem = "No existe el bloque #" + std::to_string(*number) + ": " +
                         (blocks.size() == 1 ? std::string{"solo hay el #1."}
                                             : "hay del #1 al #" + std::to_string(blocks.size()) +
                                                   ".");
        return choice;
    }
    choice.block = &blocks[static_cast<std::size_t>(*number - 1)];
    return choice;
}

std::size_t count_lines(const std::string& code) {
    if (code.empty()) {
        return 0;
    }
    const auto breaks = static_cast<std::size_t>(std::count(code.begin(), code.end(), '\n'));
    return code.back() == '\n' ? breaks : breaks + 1;
}

std::string describe_code_block(const CodeBlock& block) {
    const std::size_t lines = count_lines(block.code);
    std::string out = "el bloque #" + std::to_string(block.number) + " (";
    if (!block.info.empty()) {
        // El lenguaje sale del modelo: filtrado y en una línea.
        out += md::sanitize(block.info) + ", ";
    }
    out += std::to_string(lines) + (lines == 1 ? " línea)" : " líneas)");
    return out;
}

void CodeBlockIndex::update(const std::vector<Entry>& entries) {
    bool changed = cache_.size() != entries.size();
    cache_.resize(entries.size());
    for (std::size_t i = 0; i < entries.size(); ++i) {
        Cached& cached = cache_[i];
        const Entry& entry = entries[i];
        const bool assistant = entry.kind == EntryKind::Assistant;
        if (cached.assistant != assistant || (assistant && cached.source != entry.text)) {
            cached.assistant = assistant;
            cached.source = assistant ? entry.text : std::string{};
            cached.blocks = assistant ? code_blocks_of(md::parse(entry.text))
                                      : std::vector<CodeBlock>{};
            changed = true;
        }
    }
    if (!changed) {
        return; // Sin cambios no se copian los bloques otra vez.
    }
    blocks_.clear();
    for (std::size_t i = 0; i < cache_.size(); ++i) {
        const Cached& cached = cache_[i];
        for (const CodeBlock& block : cached.blocks) {
            CodeBlock numbered = block;
            numbered.number = static_cast<int>(blocks_.size()) + 1;
            numbered.entry = i;
            blocks_.push_back(std::move(numbered));
        }
    }
    first_numbers_ = first_code_numbers(blocks_, entries.size());
}

} // namespace chatbot::cli
