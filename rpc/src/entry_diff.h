#ifndef CHATBOT_RPC_ENTRY_DIFF_H
#define CHATBOT_RPC_ENTRY_DIFF_H

#include "conversation.h"

#include <cstddef>
#include <string>
#include <vector>

namespace chatbot::rpc {

/// Lo que ya se emitió de las entradas de una conversación: cuántas había y
/// la última (su largo y sus marcas). Copiarlo cuesta lo mismo sin importar
/// el largo de la conversación.
///
/// Basta porque hoy Conversation cumple estas suposiciones:
///  1. las entradas solo se agregan al final; nunca se borran ni se mueven;
///  2. de las ya existentes solo cambia la última (la respuesta en curso):
///     su texto crece por el final y sus marcas (in_progress, incomplete,
///     cancelled, note) cambian al terminar; kind y sources nunca cambian;
///  3. abrir o crear otra conversación reemplaza el objeto: quien llama
///     empieza otra vez desde EntriesSeen{} (o remember de la nueva).
/// Si una dejara de cumplirse, diff_entries tendría que cambiar.
struct EntriesSeen {
    std::size_t count = 0;
    std::size_t last_text_size = 0;
    bool last_in_progress = false;
    bool last_incomplete = false;
    bool last_cancelled = false;
    std::string last_note;
};

/// Qué cambió desde EntriesSeen.
struct EntriesDiff {
    /// El texto de la última entrada ya vista creció (índice seen.count - 1,
    /// el texto nuevo desde seen.last_text_size).
    bool grown = false;
    /// Cambiaron las marcas de la última entrada ya vista.
    bool updated = false;
    /// Entradas nuevas: [seen.count, added_end).
    std::size_t added_end = 0;

    [[nodiscard]] bool added(const EntriesSeen& seen) const { return added_end > seen.count; }
};

/// El estado de entries para la siguiente comparación.
[[nodiscard]] EntriesSeen remember(const std::vector<cli::Entry>& entries);

/// Compara lo visto con entries (con las suposiciones de EntriesSeen).
[[nodiscard]] EntriesDiff diff_entries(const EntriesSeen& seen,
                                       const std::vector<cli::Entry>& entries);

} // namespace chatbot::rpc

#endif // CHATBOT_RPC_ENTRY_DIFF_H
