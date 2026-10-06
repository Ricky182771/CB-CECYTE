#ifndef CHATBOT_CLI_SYSTEM_PROMPT_H
#define CHATBOT_CLI_SYSTEM_PROMPT_H

#include "chatbot/result.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace chatbot::cli {

/// Instrucciones de sistema cuando "system_prompt" no está en config.json.
inline constexpr std::string_view kDefaultSystemPrompt =
    "Eres un asistente útil. Responde en español, de forma clara y concisa.";

/// Tamaño máximo de las instrucciones, en bytes UTF-8.
inline constexpr std::size_t kMaxSystemPromptBytes = 8000;

/// Instrucciones que se usan y el aviso para la línea de estado, si hay.
struct ResolvedSystemPrompt {
    std::string text;    ///< Vacío: sin mensaje de sistema.
    std::string warning; ///< Vacío si no hay nada que avisar.
};

/// Interpreta lo que devolvió load_system_prompt_value: llave ausente → las
/// predeterminadas; una cadena (también vacía) → esa, tal cual; un error
/// (llave que no es cadena, JSON inválido) → las predeterminadas con el
/// mensaje del error como aviso.
[[nodiscard]] ResolvedSystemPrompt resolve_system_prompt(
    const Result<std::optional<std::string>>& loaded);

/// Revisa las instrucciones antes de guardarlas: como máximo
/// kMaxSystemPromptBytes bytes, UTF-8 válido y sin controles C0 (salvo
/// salto de línea y tabulador), DEL ni C1. Devuelve el motivo en español
/// para rechazar el guardado, o nullopt si son válidas. No las corrige.
[[nodiscard]] std::optional<std::string> validate_system_prompt(std::string_view text);

/// Aviso si las instrucciones miden history_limit_bytes o más (y el límite
/// no es 0): el recorte del historial nunca las quita, así que dejarían poco
/// o nada de espacio para la conversación. Nullopt si no hay que avisar.
[[nodiscard]] std::optional<std::string> system_prompt_limit_warning(
    std::string_view text, std::size_t history_limit_bytes);

/// Lo que se guarda en config.json: nullopt (borrar la llave) si el texto es
/// el predeterminado; si no, el texto.
[[nodiscard]] std::optional<std::string> system_prompt_to_store(std::string_view text);

} // namespace chatbot::cli

#endif // CHATBOT_CLI_SYSTEM_PROMPT_H
