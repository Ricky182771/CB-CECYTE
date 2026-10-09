#include "request_runner.h"

#include "search_context.h"

#include "chatbot/error.h"
#include "chatbot/history.h"

#include <algorithm>
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

bool RequestRunner::launch(std::function<void(const CancelToken&)> work) {
    if (state_->busy) {
        return false;
    }
    if (worker_.joinable()) {
        worker_.join(); // El anterior ya terminó: su on_done ya se ejecutó.
    }
    token_ = std::make_unique<CancelToken>();
    state_->busy = true;
    state_->cancel_requested = false;
    worker_ = std::thread([work = std::move(work), token = token_.get()] { work(*token); });
    return true;
}

bool RequestRunner::start(std::vector<Message> history, OnDelta on_delta, OnDone on_done) {
    return launch([&client = client_, limit = history_limit_bytes_, post = post_, state = state_,
                   history = std::move(history), on_delta = std::move(on_delta),
                   on_done = std::move(on_done)](const CancelToken& token) {
        complete(client, limit, post, state, token, history, on_delta, on_done);
    });
}

bool RequestRunner::start_with_search(std::vector<Message> history, std::string query,
                                      std::shared_ptr<SearchProvider> search_provider,
                                      std::string today, OnSearchDone on_search_done,
                                      OnDelta on_delta, OnDone on_done) {
    return launch([&client = client_, limit = history_limit_bytes_, post = post_, state = state_,
                   history = std::move(history), query = std::move(query),
                   provider = std::move(search_provider), today = std::move(today),
                   on_search_done = std::move(on_search_done), on_delta = std::move(on_delta),
                   on_done = std::move(on_done)](const CancelToken& token) mutable {
        const Result<SearchResponse> found = provider->search(query, &token);
        if (found.is_error()) {
            // Incluye la cancelación durante la búsqueda: TavilySearch la
            // devuelve como ErrorKind::Cancelled.
            post_done(post, state, on_done, found.error(), 0);
            return;
        }
        if (found.value().results.empty()) {
            post_done(post, state, on_done,
                      ChatError{ErrorKind::NoSearchResults, 0, std::string{kNoSearchResults},
                                std::nullopt},
                      0);
            return;
        }

        SearchContext context;
        context.response = trim_search_response(found.value());
        context.block = format_search_context(context.response, today, generate_search_nonce());
        // El bloque reemplaza al texto del comando: un solo mensaje User.
        if (!history.empty() && history.back().role == Role::User) {
            history.back().content = context.block;
        } else {
            history.push_back(Message{Role::User, context.block});
        }
        post([on_search_done, context] { on_search_done(context); });

        complete(client, limit, post, state, token, history, on_delta, on_done);
    });
}

void RequestRunner::complete(ChatClient& client, std::size_t limit, const Post& post,
                             const std::shared_ptr<State>& state, const CancelToken& token,
                             const std::vector<Message>& history, const OnDelta& on_delta,
                             const OnDone& on_done) {
    bool delivered = false;
    const auto deliver = [&](std::string_view delta) {
        delivered = true;
        // Copia: el string_view deja de ser válido al regresar.
        post([on_delta, text = std::string{delta}] { on_delta(text); });
        return true;
    };

    const TrimResult first = trim_history(history, limit);
    std::size_t dropped = first.dropped;
    Result<CompletionInfo> result = client.complete_stream(first.messages, deliver, &token);

    // Reintento por contexto: una sola vez, si el servidor rechazó la
    // petición antes de mandar texto y recortar a la mitad quita algo más.
    if (result.is_error() && result.error().kind == ErrorKind::InvalidRequest && !delivered &&
        !token.is_cancelled()) {
        // Mitad de min(límite, tamaño real): si el historial ya es menor
        // que el límite, recortar a la mitad del límite no quitaría nada.
        const std::size_t current = total_bytes(first.messages);
        const std::size_t base = limit > 0 ? std::min(limit, current) : current;
        // trim_history interpreta 0 como "sin límite": el mínimo es 1.
        const TrimResult second = trim_history(history, std::max<std::size_t>(base / 2, 1));
        if (second.dropped > first.dropped) {
            dropped = second.dropped;
            result = client.complete_stream(second.messages, deliver, &token);
        }
    }

    post_done(post, state, on_done, std::move(result), dropped);
}

void RequestRunner::post_done(const Post& post, const std::shared_ptr<State>& state,
                              const OnDone& on_done, Result<CompletionInfo> result,
                              std::size_t dropped) {
    post([state, on_done, result = std::move(result), dropped] {
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
}

} // namespace chatbot::cli
