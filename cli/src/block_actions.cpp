#include "block_actions.h"

#include "conversation_export.h"
#include "conversation_store.h"
#include "downloads.h"

#include <optional>
#include <utility>

namespace chatbot::cli {

namespace {

/// Escribe un archivo nuevo en <base>/chatbot. done: el inicio del aviso
/// ("Guardado en"); warning va al final (p. ej. incomplete_warning).
ActionResult write_download(const std::string& base, const std::string& home,
                            std::string_view name, std::string_view content,
                            std::string_view done, const std::string& warning = {}) {
    const WriteResult dir = ensure_download_dir(base);
    if (!dir.error.empty()) {
        return {dir.error, true};
    }
    const WriteResult written = write_new_file(dir.path, name, content);
    if (!written.error.empty()) {
        return {written.error, true};
    }
    return {std::string{done} + " " + display_path(written.path, home) +
                (warning.empty() ? "" : "." + warning),
            false};
}

} // namespace

ClipboardAccess real_clipboard() {
    const std::optional<std::string> tmux = environment_value("TMUX");
    return ClipboardAccess{
        clipboard_methods(environment_value, find_in_path), tmux.has_value() && !tmux->empty(),
        [](const std::vector<std::string>& argv, std::string_view text) {
            return run_with_input(argv, text);
        },
        write_to_terminal};
}

ActionResult copy_block(const CodeBlock& block, const ClipboardAccess& clipboard) {
    const CopyResult copied = copy_to_clipboard(block.code, clipboard.methods, clipboard.tmux,
                                                clipboard.run, clipboard.write);
    const std::string what = describe_code_block(block);
    const std::string number = std::to_string(block.number);
    if (!copied.copied) {
        return {copied.too_long
                    ? "No se pudo copiar " + what + ": pasa de 100 000 bytes, el límite de OSC 52, " +
                          "y no hay otro método. Usa /guardar " + number + "."
                    : "No se pudo copiar " + what + ". Usa /guardar " + number + ".",
                true};
    }
    if (copied.fallback) {
        return {"Copiado " + what + " con OSC 52; si tu terminal no lo soporta, usa /guardar " +
                    number + "." + incomplete_warning(block),
                false};
    }
    return {"Copiado " + what + " con " + copied.method + "." + incomplete_warning(block), false};
}

ActionResult save_block(const CodeBlock& block, std::string_view file_name,
                        const std::string& download_base, const std::string& home) {
    if (!file_name.empty()) {
        if (std::optional<std::string> reason = validate_file_name(file_name)) {
            return {std::move(*reason), true};
        }
    }
    return write_download(download_base, home, block_file_name(file_name, block.number, block.info),
                          with_final_newline(block.code), "Guardado en",
                          incomplete_warning(block));
}

ActionResult export_conversation(const Conversation& conversation, std::time_t now,
                                 const std::string& download_base, const std::string& home) {
    if (!conversation.has_turns()) {
        return {"Todavía no hay respuestas que exportar.", true};
    }
    const std::string markdown = export_markdown(conversation.to_stored(format_iso8601(now)),
                                                 export_date(format_iso8601(now)));
    return write_download(download_base, home, export_file_name(conversation.id(), now), markdown,
                          "Conversación exportada a");
}

} // namespace chatbot::cli
