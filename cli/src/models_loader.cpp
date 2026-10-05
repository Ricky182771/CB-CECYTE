#include "models_loader.h"

#include "chatbot/chat_client.h"

#include <utility>

namespace chatbot::cli {

ModelsLoader::ModelsLoader(TransportFactory make_transport, Post post)
    : make_transport_(std::move(make_transport)), post_(std::move(post)),
      state_(std::make_shared<State>()) {}

ModelsLoader::~ModelsLoader() {
    cancel();
    for (const std::unique_ptr<Job>& job : retired_) {
        job->thread.join();
    }
}

void ModelsLoader::reap() {
    std::erase_if(retired_, [](const std::unique_ptr<Job>& job) {
        if (!job->done.load()) {
            return false;
        }
        job->thread.join(); // Ya terminó: no espera.
        return true;
    });
}

void ModelsLoader::cancel() {
    ++state_->generation; // Lo que llegue de la petición actual se descarta.
    state_->busy = false;
    if (current_ != nullptr) {
        current_->token.cancel();
        retired_.push_back(std::move(current_));
    }
    reap();
}

void ModelsLoader::start(Config config, OnDone on_done) {
    cancel();
    state_->busy = true;
    const std::uint64_t generation = state_->generation;
    current_ = std::make_unique<Job>();
    Job* job = current_.get();
    std::unique_ptr<Transport> transport = make_transport_();
    // El Job vive hasta que se une su hilo (cancel/reap o el destructor).
    job->thread = std::thread([job, post = post_, state = state_, generation,
                               config = std::move(config), transport = std::move(transport),
                               on_done = std::move(on_done)]() mutable {
        Result<std::vector<std::string>> result = [&] {
            ChatClient client(std::move(config), std::move(transport));
            return client.list_models(&job->token);
        }();
        // state solo se lee y escribe en el hilo de la interfaz (en la tarea).
        post([state, generation, on_done = std::move(on_done), result = std::move(result)] {
            if (state->generation != generation) {
                return; // Se canceló o se pidió otra lista.
            }
            ++state->generation;
            state->busy = false;
            on_done(result);
        });
        job->done.store(true);
    });
}

} // namespace chatbot::cli
