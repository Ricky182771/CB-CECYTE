#ifndef CHATBOT_CLI_CODE_BLOCKS_H
#define CHATBOT_CLI_CODE_BLOCKS_H

#include "conversation.h"
#include "markdown.h"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace chatbot::cli {

/// Un bloque de código de una respuesta del asistente.
struct CodeBlock {
    int number = 0;         ///< Desde 1 en toda la conversación, en orden.
    std::string info;       ///< Lenguaje del bloque (Block::info), o vacío.
    std::string code;       ///< Texto del bloque, tal como lo dio md::parse.
    std::size_t entry = 0;  ///< Índice de la entrada donde está.
};

/// Bloques de código de un documento en el orden en que md::render los
/// dibuja: los bloques en orden (también dentro de listas, citas y
/// alertas) y después las notas al pie. Numerados desde 1.
[[nodiscard]] std::vector<CodeBlock> code_blocks_of(const md::Document& document);

/// Bloques de código de las respuestas del asistente (EntryKind::Assistant,
/// también la que sigue llegando), numerados desde 1 en toda la
/// conversación. Función pura: parsea cada respuesta con md::parse.
[[nodiscard]] std::vector<CodeBlock> collect_code_blocks(const std::vector<Entry>& entries);

/// Número que lleva el primer bloque de código de cada entrada (lo que
/// recibe HistoryView::render), a partir de collect_code_blocks: una
/// entrada sin bloques lleva el número que tendría su primer bloque.
[[nodiscard]] std::vector<int> first_code_numbers(const std::vector<CodeBlock>& blocks,
                                                  std::size_t entry_count);

/// Bloque que piden /copiar y /guardar: block apunta a él, o problem
/// explica por qué no hay (sin bloques o número fuera de rango).
struct CodeBlockChoice {
    const CodeBlock* block = nullptr;
    std::string problem;
};

/// El bloque con ese número, o el último si number es nullopt.
[[nodiscard]] CodeBlockChoice choose_code_block(const std::vector<CodeBlock>& blocks,
                                                std::optional<int> number);

/// Líneas del texto del bloque (sin contar el salto de línea final).
[[nodiscard]] std::size_t count_lines(const std::string& code);

/// "el bloque #3 (cpp, 24 líneas)", o "el bloque #3 (1 línea)" sin lenguaje.
[[nodiscard]] std::string describe_code_block(const CodeBlock& block);

/// collect_code_blocks con caché por entrada: solo vuelve a parsear una
/// respuesta si su texto cambió (mientras llega una respuesta, la interfaz
/// lo llama en cada cuadro). Da el mismo resultado que collect_code_blocks.
/// Se usa solo desde el hilo de la interfaz.
class CodeBlockIndex {
public:
    /// Actualiza el índice con las entradas actuales.
    void update(const std::vector<Entry>& entries);

    [[nodiscard]] const std::vector<CodeBlock>& blocks() const { return blocks_; }
    [[nodiscard]] const std::vector<int>& first_numbers() const { return first_numbers_; }

private:
    struct Cached {
        bool assistant = false;
        std::string source;
        std::vector<CodeBlock> blocks; ///< Numerados desde 1 dentro de la entrada.
    };

    std::vector<Cached> cache_;
    std::vector<CodeBlock> blocks_;
    std::vector<int> first_numbers_;
};

} // namespace chatbot::cli

#endif // CHATBOT_CLI_CODE_BLOCKS_H
