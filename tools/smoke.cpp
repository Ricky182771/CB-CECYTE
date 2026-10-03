// smoke.cpp — programa desechable para probar el núcleo a mano (sección 5).
//
// Uso (el usuario lo corre; las pruebas automáticas nunca llaman a la API):
//
//   export CHAT_API_KEY="nvapi-..."
//   export CHAT_MODEL="meta/llama-3.1-8b-instruct"
//   ./build/dev/tools/smoke "¿Cómo estás?"           # respuesta completa
//   ./build/dev/tools/smoke --stream "¿Cómo estás?"  # respuesta por fragmentos
//
// Si no se pasa el mensaje como argumento, se usa uno de ejemplo.
// Muestra la respuesta del asistente o el tipo de error; nunca imprime la key.

#include "chatbot/chat_client.h"
#include "chatbot/config.h"
#include "chatbot/curl_transport.h"
#include "chatbot/error.h"
#include "chatbot/result.h"
#include "chatbot/types.h"

#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

/// Imprime un error con su tipo y detalles conocidos.
void print_error(const chatbot::ChatError& error) {
    std::cerr << "[" << chatbot::error_kind_label(error.kind) << "]"
              << " http_status=" << error.http_status;
    if (error.retry_after.has_value()) {
        std::cerr << " retry_after=" << error.retry_after->count() << "s";
    }
    std::cerr << ": " << error.message << '\n';
}

} // namespace

int main(int argc, char* argv[]) {
    bool stream = false;
    int prompt_index = 1;
    if (argc > 1 && std::string{argv[1]} == "--stream") {
        stream = true;
        prompt_index = 2;
    }
    const std::string prompt =
        argc > prompt_index ? std::string{argv[prompt_index]}
                            : std::string{"Salúdame en una frase."};

    const chatbot::ConfigOptions options; // Lee el entorno real.
    const chatbot::Result<chatbot::Config> config = chatbot::load_config(options);
    if (config.is_error()) {
        print_error(config.error());
        return 1;
    }

    std::vector<chatbot::Message> messages;
    messages.push_back(chatbot::Message{chatbot::Role::System, "Eres un asistente breve."});
    messages.push_back(chatbot::Message{chatbot::Role::User, prompt});

    chatbot::ChatClient client(config.value(),
                               std::make_unique<chatbot::CurlTransport>());

    if (!stream) {
        const chatbot::Result<std::string> response = client.complete(messages);
        if (response.is_error()) {
            print_error(response.error());
            return 1;
        }
        std::cout << response.value() << '\n';
        return 0;
    }

    // Streaming: imprime cada fragmento en cuanto llega, sin salto de línea.
    const chatbot::Result<void> response = client.complete_stream(
        messages, [](std::string_view delta) {
            std::cout << delta << std::flush;
            return true;
        });
    if (response.is_error()) {
        std::cerr << '\n';
        print_error(response.error());
        return 1;
    }
    std::cout << '\n';
    return 0;
}
