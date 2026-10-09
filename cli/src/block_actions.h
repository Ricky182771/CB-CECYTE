#ifndef CHATBOT_CLI_BLOCK_ACTIONS_H
#define CHATBOT_CLI_BLOCK_ACTIONS_H

#include "clipboard.h"
#include "code_blocks.h"
#include "conversation.h"

#include <ctime>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace chatbot::cli {

/// Resultado de copiar, guardar o exportar: el aviso para el usuario y si
/// es un error. Quien llama decide dónde mostrarlo (el historial para los
/// comandos, la línea de estado para los botones).
struct ActionResult {
    std::string message;
    bool error = false;
};

/// Sin carpeta de descargas (resolve_download_dir dio nullopt).
inline constexpr std::string_view kNoDownloadDir =
    "No se encontró la carpeta de descargas (define HOME o CHAT_DOWNLOAD_DIR).";

/// Cómo se escriben los avisos: Long para los comandos (van al historial),
/// Short para los botones (van a la línea de estado, de una fila): solo lo
/// que el botón no dice, con lo importante primero ("#1 copiado con
/// wl-copy", "⚠ #1 incompleto (aún no termina) · copiado con wl-copy").
enum class Wording { Long, Short };

/// Qué hace un botón de un bloque de código.
enum class BlockAction { Copy, Save };

/// Lo que copy_block necesita para copiar, inyectable para probarla sin
/// lanzar procesos ni escribir en la terminal (ver copy_to_clipboard).
struct ClipboardAccess {
    std::vector<ClipboardMethod> methods;
    bool tmux = false;
    ProgramRunner run;
    TerminalWriter write;
};

/// El portapapeles real: clipboard_methods con el entorno del proceso,
/// run_with_input y write_to_terminal.
[[nodiscard]] ClipboardAccess real_clipboard();

/// Copia el bloque. "Copiado el bloque #3 (cpp, 24 líneas) con wl-copy.",
/// con el aviso de OSC 52 como último recurso o de bloque incompleto
/// (incomplete_warning); si no se pudo, un error que sugiere /guardar. Con
/// Wording::Short: "#3 copiado con wl-copy", "#3 copiado con OSC 52 · si no
/// pega, usa [Guardar]", "⚠ #3 incompleto (se canceló) · copiado con ..." o
/// "No se pudo copiar #3 · usa [Guardar]".
[[nodiscard]] ActionResult copy_block(const CodeBlock& block, const ClipboardAccess& clipboard,
                                      Wording wording = Wording::Long);

/// Guarda el bloque en <download_base>/chatbot (que crea si falta) con
/// file_name, o "bloque-<n>.<ext>" si está vacío (block_file_name), sin
/// sobrescribir. Un file_name que no pasa validate_file_name es un error
/// (los comandos lo revisan antes, para mostrarlo en la línea de estado).
/// home abrevia la ruta del aviso con "~". Con Wording::Short: "#3 guardado
/// en ~/Descargas/chatbot/bloque-3.cpp", con "⚠ #3 incompleto (...) · "
/// delante si hace falta; los errores de escritura no cambian.
[[nodiscard]] ActionResult save_block(const CodeBlock& block, std::string_view file_name,
                                      const std::string& download_base, const std::string& home,
                                      Wording wording = Wording::Long);

/// Lo que hace un botón del bloque con ese número (de collect_code_blocks):
/// copy_block, o save_block con el nombre por defecto en download_base
/// (resolve_download_dir; nullopt es un error), con Wording::Short. Un
/// número que ya no existe es un error (choose_code_block).
[[nodiscard]] ActionResult run_block_action(const std::vector<CodeBlock>& blocks, int number,
                                            BlockAction action, const ClipboardAccess& clipboard,
                                            const std::optional<std::string>& download_base,
                                            const std::string& home);

/// Exporta los pares guardados de la conversación a Markdown en
/// <download_base>/chatbot (export_markdown, export_file_name). Sin pares es
/// un error ("Todavía no hay respuestas que exportar.").
[[nodiscard]] ActionResult export_conversation(const Conversation& conversation, std::time_t now,
                                               const std::string& download_base,
                                               const std::string& home);

} // namespace chatbot::cli

#endif // CHATBOT_CLI_BLOCK_ACTIONS_H
