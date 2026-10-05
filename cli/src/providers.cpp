#include "providers.h"

#include <array>

namespace chatbot::cli {

namespace {

// "/models": confirmado en la documentación de nvidia, openai, gemini (con
// prefijo "models/"), openrouter, mistral, deepseek, together y ollama; no
// documentado en anthropic ni confirmado en groq, xai y cerebras. La app lo
// intenta igual y, si falla, deja escribir el modelo a mano.
constexpr std::array kProviders{
    ProviderInfo{"nvidia", "NVIDIA NIM", "https://integrate.api.nvidia.com/v1", true},
    ProviderInfo{"openai", "OpenAI", "https://api.openai.com/v1", true},
    ProviderInfo{"gemini", "Google Gemini",
                 "https://generativelanguage.googleapis.com/v1beta/openai", true},
    ProviderInfo{"anthropic", "Anthropic (Claude)", "https://api.anthropic.com/v1", true},
    ProviderInfo{"groq", "Groq", "https://api.groq.com/openai/v1", true},
    ProviderInfo{"openrouter", "OpenRouter", "https://openrouter.ai/api/v1", true},
    ProviderInfo{"mistral", "Mistral", "https://api.mistral.ai/v1", true},
    ProviderInfo{"deepseek", "DeepSeek", "https://api.deepseek.com", true},
    ProviderInfo{"together", "Together AI", "https://api.together.ai/v1", true},
    ProviderInfo{"xai", "xAI (Grok)", "https://api.x.ai/v1", true},
    ProviderInfo{"cerebras", "Cerebras", "https://api.cerebras.ai/v1", true},
    ProviderInfo{"ollama", "Ollama (local)", "http://localhost:11434/v1", false},
    ProviderInfo{kCustomProvider, "Personalizado…", "", true},
};

} // namespace

std::span<const ProviderInfo> providers() { return kProviders; }

std::size_t provider_index(std::string_view id) {
    for (std::size_t i = 0; i < kProviders.size(); ++i) {
        if (kProviders[i].id == id) {
            return i;
        }
    }
    return kProviders.size() - 1; // "custom".
}

} // namespace chatbot::cli
