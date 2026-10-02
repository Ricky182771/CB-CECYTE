#ifndef CHATBOT_RESULT_H
#define CHATBOT_RESULT_H

#include "chatbot/error.h"

#include <cassert>
#include <utility>
#include <variant>

namespace chatbot {

/// Resultado mínimo sobre std::variant: o un valor de tipo T o un ChatError.
/// Las excepciones pueden usarse dentro de la implementación, pero nunca
/// cruzan esta API pública (sección 6).
template <typename T>
class Result {
public:
    Result(T value) : value_(std::move(value)) {}
    Result(ChatError error) : value_(std::move(error)) {}

    [[nodiscard]] bool is_ok() const { return std::holds_alternative<T>(value_); }
    [[nodiscard]] bool is_error() const { return std::holds_alternative<ChatError>(value_); }

    /// Valor de la variante exitosa. Requiere is_ok().
    [[nodiscard]] const T& value() const {
        assert(is_ok());
        return std::get<T>(value_);
    }

    /// Error de la variante fallida. Requiere is_error().
    [[nodiscard]] const ChatError& error() const {
        assert(is_error());
        return std::get<ChatError>(value_);
    }

private:
    std::variant<T, ChatError> value_;
};

} // namespace chatbot

#endif // CHATBOT_RESULT_H
