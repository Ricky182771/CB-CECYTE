#ifndef CHATBOT_RPC_LINE_READER_H
#define CHATBOT_RPC_LINE_READER_H

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace chatbot::rpc {

/// Tope de una línea del protocolo (sin el "\n").
inline constexpr std::size_t kMaxLineBytes = std::size_t{1} << 20;

/// Corta los bytes de stdin en líneas, sin depender de cómo lleguen
/// partidos. Sin hilos ni stdio.
class LineReader {
public:
    struct Item {
        enum class Kind {
            Line,     ///< Una línea, sin "\n" ni el "\r" final.
            Oversized ///< Una línea pasó del tope: se descarta hasta el siguiente "\n".
        };
        Kind kind = Kind::Line;
        std::string text; ///< Solo en Line.
    };

    explicit LineReader(std::size_t max_bytes = kMaxLineBytes) : max_bytes_(max_bytes) {}

    /// Bytes nuevos; devuelve lo que completaron, en orden. Una línea vacía
    /// (o solo "\r") no se devuelve.
    [[nodiscard]] std::vector<Item> feed(std::string_view bytes);
    /// Fin de la entrada: la última línea si no terminaba en "\n".
    [[nodiscard]] std::optional<Item> finish();

private:
    std::size_t max_bytes_;
    std::string pending_;
    bool discarding_ = false; ///< Pasó del tope y ya se avisó: se tira hasta "\n".
};

} // namespace chatbot::rpc

#endif // CHATBOT_RPC_LINE_READER_H
