#include "request_runner.h"

#include "chatbot/error.h"
#include "chatbot/history.h"

#include <algorithm>
#include <chrono>
#include <random>
#include <string_view>
#include <utility>

namespace chatbot::cli {
namespace {

std::size_t total_bytes(const std::vector<Message>& messages) {
    std::size_t total = 0;
    for (const Message& message : messages) {
        total += message.content.size();
    }
    return total;
}

/// Genera un nonce aleatorio de 16 caracteres hexadecimales.
std::string generate_nonce() {
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dist(0, UINT64_MAX);
    const uint64_t value = dist(gen);
    char buffer[17];
    std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(value));
    return std::string{buffer};
}

/// Mapea un error de búsqueda a un ChatError.
ChatError map_search_error(const ChatError& search_error) {
    // Los errores de búsqueda ya vienen como ChatError de TavilySearch.
    return search_error;
}

} // namespace

RequestRunner::RequestRunner(ChatClient& client, std::size_t history_limit_bytes, Post post)
    : client_(client), history_limit_bytes_(history_limit_bytes), post_(std::move(post)),
      state_(std::make_shared<State>()) {}

RequestRunner::~RequestRunner() {
    if (worker_.joinable()) {
        token_->cancel();
        worker_.join();
    }
}

bool RequestRunner::busy() const {
    return state_->busy;
}

void RequestRunner::cancel() {
    if (state_->busy && token_ != nullptr) {
        state_->cancel_requested = true;
        token_->cancel();
    }
}

bool RequestRunner::start(std::vector<Message> history, OnDelta on_delta, OnDone on_done) {
    if (state_->busy) {
        return false;
    }
    if (worker_.joinable()) {
        worker_.join(); // El anterior ya terminó: su on_done ya se ejecutó.
    }
    token_ = std::make_unique<CancelToken>();
    state_->busy = true;
    state_->cancel_requested = false;

    worker_ = std::thread([&client = client_, limit = history_limit_bytes_, post = post_,
                           state = state_, token = token_.get(), history = std::move(history),
                           on_delta = std::move(on_delta), on_done = std::move(on_done)] {
        bool delivered = false;
        const auto deliver = [&](std::string_view delta) {
            delivered = true;
            // Copia: el string_view deja de ser válido al regresar.
            post([on_delta, text = std::string{delta}] { on_delta(text); });
            return true;
        };

        const TrimResult first = trim_history(history, limit);
        std::size_t dropped = first.dropped;
        Result<CompletionInfo> result = client.complete_stream(first.messages, deliver, token);

        // Reintento por contexto: una sola vez, si el servidor rechazó la
        // petición antes de mandar texto y recortar a la mitad quita algo más.
        if (result.is_error() && result.error().kind == ErrorKind::InvalidRequest &&
            !delivered && !token->is_cancelled()) {
            // Mitad de min(límite, tamaño real): si el historial ya es menor
            // que el límite, recortar a la mitad del límite no quitaría nada.
            const std::size_t current = total_bytes(first.messages);
            const std::size_t base = limit > 0 ? std::min(limit, current) : current;
            // trim_history interpreta 0 como "sin límite": el mínimo es 1.
            const TrimResult second = trim_history(history, std::max<std::size_t>(base / 2, 1));
            if (second.dropped > first.dropped) {
                dropped = second.dropped;
                result = client.complete_stream(second.messages, deliver, token);
            }
        }

        post([state, on_done, result, dropped] {
            state->busy = false;
            // Si el usuario canceló después de que el hilo terminó, pero antes
            // de que este aviso llegara a la interfaz, la cancelación gana.
            if (state->cancel_requested && result.is_ok()) {
                on_done(ChatError{ErrorKind::Cancelled, 0, "Petición cancelada por el usuario.",
                                  std::nullopt},
                        dropped);
                return;
            }
            on_done(result, dropped);
        });
    });
    return true;
}

bool RequestRunner::start_with_search(std::vector<Message> history,
                                      std::string query,
                                      SearchProvider& search_provider,
                                      std::string_view today,
                                      OnSearchDone on_search_done,
                                      OnDelta on_delta,
                                      OnDone on_done) {
    if (state_->busy) {
        return false;
    }
    if (worker_.joinable()) {
        worker_.join();
    }
    token_ = std::make_unique<CancelToken>();
    state_->busy = true;
    state_->cancel_requested = false;

    worker_ = std::thread([&client = client_, &search_provider, limit = history_limit_bytes_,
                           post = post_, state = state_, token = token_.get(),
                           history = std::move(history), query = std::move(query),
                           today = std::string{today}, on_search_done = std::move(on_search_done),
                           on_delta = std::move(on_delta), on_done = std::move(on_done)] {
        // Ejecutar la búsqueda.
        Result<SearchResponse> search_result = search_provider.search(query, token);

        // Notificar el resultado de la búsqueda.
        post([on_search_done, search_result] { on_search_done(search_result); });

        // Si la búsqueda falló o se canceló, terminar.
        if (search_result.is_error()) {
            const ChatError error = map_search_error(search_result.error());
            post([state, on_done, error] {
                state->busy = false;
                on_done(error, 0);
            });
            return;
        }

        if (token->is_cancelled()) {
            post([state, on_done] {
                state->busy = false;
                on_done(ChatError{ErrorKind::Cancelled, 0, "Búsqueda cancelada por el usuario.",
                                  std::nullopt},
                        0);
            });
            return;
        }

        // Formatear el contexto de búsqueda.
        const std::string nonce = generate_nonce();
        const std::string search_context =
            format_search_context(search_result.value(), today, nonce);

        // Agregar el contexto al historial como mensaje User.
        std::vector<Message> augmented_history = history;
        augmented_history.push_back({Role::User, search_context});

        // Ejecutar la petición de completado con el historial aumentado.
        bool delivered = false;
        const auto deliver = [&](std::string_view delta) {
            delivered = true;
            post([on_delta, text = std::string{delta}] { on_delta(text); });
            return true;
        };

        const TrimResult first = trim_history(augmented_history, limit);
        std::size_t dropped = first.dropped;
        Result<CompletionInfo> result = client.complete_stream(first.messages, deliver, token);

        // Reintento por contexto.
        if (result.is_error() && result.error().kind == ErrorKind::InvalidRequest &&
            !delivered && !token->is_cancelled()) {
            const std::size_t current = total_bytes(first.messages);
            const std::size_t base = limit > 0 ? std::min(limit, current) : current;
            const TrimResult second =
                trim_history(augmented_history, std::max<std::size_t>(base / 2, 1));
            if (second.dropped > first.dropped) {
                dropped = second.dropped;
                result = client.complete_stream(second.messages, deliver, token);
            }
        }

        post([state, on_done, result, dropped] {
            state->busy = false;
            if (state->cancel_requested && result.is_ok()) {
                on_done(ChatError{ErrorKind::Cancelled, 0, "Petición cancelada por el usuario.",
                                  std::nullopt},
                        dropped);
                return;
            }
            on_done(result, dropped);
        });
    });
    return true;
}

} // namespace chatbot::cli
