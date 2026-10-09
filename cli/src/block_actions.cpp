#include "block_actions.h"

#include "conversation_export.h"
#include "conversation_store.h"
#include "downloads.h"

#include <optional>
#include <utility>

namespace chatbot::cli {

namespace {

/// Escribe un archivo nuevo en <base>/chatbot: la ruta (abreviada con
/// "~") o el error.
WriteResult write_download(const std::string& base, const std::string& home,
                           std::string_view name, std::string_view content) {
    const WriteResult dir = ensure_download_dir(base);
    if (!dir.error.empty()) {
        return dir;
    }
    WriteResult written = write_new_file(dir.path, name, content);
    if (written.error.empty()) {
        written.path = display_path(written.path, home);
    }
    return written;
}

/// Inicio de un aviso corto: "⚠ #3 incompleto (se canceló) · " si el bloque
/// está incompleto; si no, "#3 " (y el verbo sigue en minúsculas).
std::string short_prefix(const CodeBlock& block) {
    const std::string number = "#" + std::to_string(block.number);
    switch (block.complete ? IncompleteReason::None : block.reason) {
    case IncompleteReason::None:
        break;
    case IncompleteReason::Cancelled:
        return "⚠ " + number + " incompleto (se canceló) · ";
    case IncompleteReason::Error:
        return "⚠ " + number + " incompleto (se cortó) · ";
    case IncompleteReason::InProgress:
        return "⚠ " + number + " incompleto (aún no termina) · ";
    }
    return number + " ";
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

ActionResult copy_block(const CodeBlock& block, const ClipboardAccess& clipboard,
                        Wording wording) {
    const CopyResult copied = copy_to_clipboard(block.code, clipboard.methods, clipboard.tmux,
                                                clipboard.run, clipboard.write);
    if (wording == Wording::Short) {
        if (!copied.copied) {
            return {"No se pudo copiar #" + std::to_string(block.number) + " · usa [Guardar]",
                    true};
        }
        return {short_prefix(block) + "copiado con " + copied.method +
                    (copied.fallback ? " · si no pega, usa [Guardar]" : ""),
                false};
    }
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
                        const std::string& download_base, const std::string& home,
                        Wording wording) {
    if (!file_name.empty()) {
        if (std::optional<std::string> reason = validate_file_name(file_name)) {
            return {std::move(*reason), true};
        }
    }
    const WriteResult written =
        write_download(download_base, home, block_file_name(file_name, block.number, block.info),
                       with_final_newline(block.code));
    if (!written.error.empty()) {
        return {written.error, true};
    }
    if (wording == Wording::Short) {
        return {short_prefix(block) + "guardado en " + written.path, false};
    }
    const std::string warning = incomplete_warning(block);
    return {"Guardado en " + written.path + (warning.empty() ? "" : "." + warning), false};
}

ActionResult run_block_action(const std::vector<CodeBlock>& blocks, int number,
                              BlockAction action, const ClipboardAccess& clipboard,
                              const std::optional<std::string>& download_base,
                              const std::string& home) {
    const CodeBlockChoice choice = choose_code_block(blocks, number);
    if (choice.block == nullptr) {
        return {choice.problem, true};
    }
    if (action == BlockAction::Copy) {
        return copy_block(*choice.block, clipboard, Wording::Short);
    }
    if (!download_base.has_value()) {
        return {std::string{no_download_dir_message(current_os())}, true};
    }
    return save_block(*choice.block, {}, *download_base, home, Wording::Short);
}

ActionResult export_conversation(const Conversation& conversation, std::time_t now,
                                 const std::string& download_base, const std::string& home) {
    if (!conversation.has_turns()) {
        return {"Todavía no hay respuestas que exportar.", true};
    }
    const std::string markdown = export_markdown(conversation.to_stored(format_iso8601(now)),
                                                 export_date(format_iso8601(now)));
    const WriteResult written =
        write_download(download_base, home, export_file_name(conversation.id(), now), markdown);
    if (!written.error.empty()) {
        return {written.error, true};
    }
    return {"Conversación exportada a " + written.path, false};
}

} // namespace chatbot::cli
