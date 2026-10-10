// Lógica de la sección "Proveedor de IA" (sin FTXUI): tabla, cambio de
// proveedor, URL editable solo en "custom", enmascarado, campos bloqueados
// por CHAT_*, filtro de modelos y cambios sin guardar.

#include "provider_settings.h"
#include "providers.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <optional>
#include <string>
#include <vector>

namespace {

using chatbot::ConfigFileValues;
using chatbot::Credentials;
using chatbot::cli::ModelsState;
using chatbot::cli::ProviderSettings;
using chatbot::cli::provider_index;
using chatbot::cli::SettingsEnv;

constexpr const char* kSecret = "sk-clave-secreta-de-prueba-1234";

ConfigFileValues saved(std::string provider, std::string base_url, std::string model) {
    return ConfigFileValues{std::move(provider), std::move(base_url), std::move(model)};
}

Credentials credentials_with(std::string id, std::string key) {
    Credentials credentials;
    credentials.keys[std::move(id)] = std::move(key);
    return credentials;
}

} // namespace

TEST_CASE("proveedores: tabla con custom al final y ids únicos", "[proveedor][ajustes]") {
    const auto table = chatbot::cli::providers();
    REQUIRE(table.size() == 13);
    CHECK(table.back().id == chatbot::cli::kCustomProvider);
    CHECK(table.back().base_url.empty());
    for (std::size_t i = 0; i < table.size(); ++i) {
        CAPTURE(table[i].id);
        CHECK(provider_index(table[i].id) == i);
        if (i + 1 < table.size()) {
            // Toda URL de la tabla es válida (http solo en Ollama, local).
            CHECK_FALSE(chatbot::validate_base_url(table[i].base_url).has_value());
            CHECK(table[i].needs_key == (table[i].id != "ollama"));
        }
    }
    CHECK(provider_index("no-existe") == table.size() - 1);
    CHECK(table[provider_index("gemini")].base_url ==
          "https://generativelanguage.googleapis.com/v1beta/openai");
}

TEST_CASE("ajustes: el proveedor inicial sale de config.json", "[proveedor][ajustes]") {
    CHECK(ProviderSettings(saved("", "", ""), {}, {}).provider_info().id == "nvidia");
    CHECK(ProviderSettings(saved("", "https://api.openai.com/v1", ""), {}, {})
              .provider_info()
              .id == "openai");
    const ProviderSettings custom(saved("", "http://localhost:8080/v1", "m"), {}, {});
    CHECK(custom.provider_info().id == "custom");
    CHECK(custom.base_url() == "http://localhost:8080/v1");
    CHECK(custom.model() == "m");
    CHECK_FALSE(custom.dirty());
}

TEST_CASE("ajustes: cambiar de proveedor rellena la URL y la key guardada",
          "[proveedor][ajustes]") {
    ProviderSettings settings(saved("nvidia", "https://integrate.api.nvidia.com/v1", "m1"),
                              credentials_with("gemini", kSecret), {});
    CHECK(settings.key_status().empty()); // NVIDIA sin key guardada.
    settings.set_key("escrita");
    settings.select_provider(provider_index("gemini"));
    CHECK(settings.base_url() == "https://generativelanguage.googleapis.com/v1beta/openai");
    CHECK(settings.key().empty()); // La key escrita era de otro proveedor.
    CHECK(settings.key_status() == "guardada: …1234");
    CHECK(settings.effective_key() == kSecret);
    CHECK(settings.model().empty());
    // De regreso al guardado, vuelve su modelo.
    settings.select_provider(provider_index("nvidia"));
    CHECK(settings.model() == "m1");
    // Ollama: local, key opcional.
    settings.select_provider(provider_index("ollama"));
    CHECK(settings.base_url() == "http://localhost:11434/v1");
    CHECK(settings.key_status() == "opcional en local");
    CHECK(settings.can_request_models());
}

TEST_CASE("ajustes: la URL solo se edita en Personalizado", "[proveedor][ajustes]") {
    ProviderSettings settings(saved("openai", "https://api.openai.com/v1", ""), {}, {});
    CHECK_FALSE(settings.base_url_editable());
    CHECK_FALSE(settings.set_base_url("https://otra/v1"));
    CHECK(settings.base_url() == "https://api.openai.com/v1");

    settings.select_provider(provider_index("custom"));
    CHECK(settings.base_url_editable());
    CHECK(settings.base_url().empty());
    CHECK(settings.set_base_url("https://mi-servidor/v1"));
    settings.set_key(kSecret);
    // La key del endpoint propio va por su URL.
    REQUIRE(settings.credential_update().has_value());
    CHECK(settings.credential_update()->first == "custom:https://mi-servidor/v1");
    // Al regresar a "custom" se recupera la última URL escrita.
    settings.select_provider(provider_index("openai"));
    settings.select_provider(provider_index("custom"));
    CHECK(settings.base_url() == "https://mi-servidor/v1");
    CHECK(settings.provider_label() == "https://mi-servidor/v1");
}

TEST_CASE("ajustes: el enmascarado nunca expone más de 4 caracteres", "[proveedor][ajustes]") {
    using chatbot::cli::mask_key;
    CHECK(mask_key(kSecret) == "…1234");
    CHECK(mask_key("") == "…");
    CHECK(mask_key("abcd") == "…");    // Corta: no se muestra nada.
    CHECK(mask_key("abcdefg") == "…"); // 7 < 8.
    CHECK(mask_key("abcdefgh") == "…efgh");
    CHECK(mask_key("abcdef\x1b[2J") == "…"); // Controles: nada.
    for (const std::string& key : {std::string{kSecret}, std::string{"nvapi-0123456789abcdef"},
                                  std::string{"x"}, std::string(100, 'k')}) {
        const std::string masked = mask_key(key);
        CHECK(masked.size() <= std::string{"…"}.size() + 4);
    }
    // Ni el estado ni nada de lo que se dibuja trae la key entera.
    ProviderSettings settings(saved("nvidia", "", ""), credentials_with("nvidia", kSecret), {});
    CHECK(settings.key_status().find("clave-secreta") == std::string::npos);
    SettingsEnv env;
    env.api_key = kSecret;
    ProviderSettings locked(saved("nvidia", "", ""), {}, env);
    CHECK(locked.key_status() == "(definido por CHAT_API_KEY) …1234");
}

TEST_CASE("ajustes: CHAT_* bloquean sus campos", "[proveedor][ajustes]") {
    SettingsEnv env;
    env.base_url = "https://desde-entorno/v1";
    env.model = "modelo-entorno";
    env.api_key = "clave-del-entorno-9999";
    ProviderSettings settings(saved("custom", "https://guardada/v1", "guardado"),
                              credentials_with("custom:https://desde-entorno/v1", kSecret), env);
    CHECK(settings.base_url_locked());
    CHECK_FALSE(settings.base_url_editable());
    CHECK_FALSE(settings.set_base_url("https://otra/v1"));
    CHECK(settings.effective_base_url() == "https://desde-entorno/v1");
    CHECK(settings.base_url() == "https://guardada/v1");
    CHECK(settings.key_locked());
    CHECK_FALSE(settings.set_key("nueva"));
    CHECK(settings.effective_key() == "clave-del-entorno-9999");
    CHECK_FALSE(settings.credential_update().has_value());
    CHECK(settings.model_locked());
    CHECK_FALSE(settings.set_model("otro"));
    settings.models_loaded({"a", "b"});
    CHECK_FALSE(settings.pick_highlighted());
    CHECK(settings.effective_model() == "modelo-entorno");
    // Al guardar, el modelo de config.json no se pisa con el del entorno.
    CHECK(settings.file_values().model == "guardado");
    const chatbot::Config config = settings.request_config({});
    CHECK(config.base_url == "https://desde-entorno/v1");
    CHECK(config.api_key == "clave-del-entorno-9999");
    CHECK(config.model == "modelo-entorno");
    CHECK_FALSE(settings.validate().has_value());
}

TEST_CASE("ajustes: filtro de modelos, resaltado y elección", "[proveedor][ajustes]") {
    ProviderSettings settings(saved("gemini", "", "gemini-3.5-flash-lite"), {}, {});
    CHECK(settings.models_state() == ModelsState::Idle);
    settings.models_loading();
    CHECK(settings.models_state() == ModelsState::Loading);
    settings.models_loaded({"gemini-3.5-flash", "gemini-3.5-flash-lite", "Gemini-Pro", "embed"});
    CHECK(settings.models_state() == ModelsState::Loaded);
    CHECK(settings.highlighted() == 1); // Empieza en el elegido.

    settings.set_filter("GEMINI");
    CHECK(settings.filtered_models() ==
          std::vector<std::string>{"gemini-3.5-flash", "gemini-3.5-flash-lite", "Gemini-Pro"});
    settings.set_filter("flash");
    CHECK(settings.filtered_models().size() == 2);
    settings.move_highlight(5);
    CHECK(settings.highlighted() == 1);
    settings.move_highlight(-9);
    CHECK(settings.highlighted() == 0);
    REQUIRE(settings.pick_highlighted());
    CHECK(settings.model() == "gemini-3.5-flash");
    settings.set_filter("nada-coincide");
    CHECK(settings.filtered_models().empty());
    CHECK_FALSE(settings.pick_highlighted());

    // Si /models falla, el modelo se escribe a mano.
    settings.models_failed("[error de autenticación] Unauthorized");
    CHECK(settings.models_state() == ModelsState::Failed);
    CHECK(settings.models_error() == "[error de autenticación] Unauthorized");
    CHECK(settings.set_model("claude-a-mano"));
    CHECK(settings.effective_model() == "claude-a-mano");
}

TEST_CASE("ajustes: cambios sin guardar y validación", "[proveedor][ajustes]") {
    ProviderSettings settings(saved("nvidia", "https://integrate.api.nvidia.com/v1", "m"),
                              credentials_with("nvidia", kSecret), {});
    CHECK_FALSE(settings.dirty());
    CHECK_FALSE(settings.validate().has_value());
    settings.set_filter("x"); // El filtro no es un cambio.
    CHECK_FALSE(settings.dirty());
    settings.set_model("otro");
    CHECK(settings.dirty());
    settings.set_model("m");
    CHECK_FALSE(settings.dirty());
    settings.set_key("nueva-clave-escrita");
    CHECK(settings.dirty());
    settings.set_key("");
    settings.select_provider(provider_index("openai"));
    CHECK(settings.dirty());
    // Sin key ni modelo para OpenAI: no se puede guardar.
    REQUIRE(settings.validate().has_value());
    CHECK_THAT(*settings.validate(), Catch::Matchers::ContainsSubstring("API key"));
    CHECK_FALSE(settings.can_request_models());
    settings.set_key("sk-openai-escrita-0000");
    REQUIRE(settings.validate().has_value());
    CHECK_THAT(*settings.validate(), Catch::Matchers::ContainsSubstring("modelo"));
    settings.set_model("gpt");
    CHECK_FALSE(settings.validate().has_value());

    // Guardar: lo actual pasa a ser lo guardado; la key queda en credentials.
    const ConfigFileValues values = settings.file_values();
    CHECK(values.provider == "openai");
    CHECK(values.base_url == "https://api.openai.com/v1");
    CHECK(values.model == "gpt");
    settings.mark_saved();
    CHECK_FALSE(settings.dirty());
    CHECK(settings.key().empty());
    CHECK(settings.key_status() == "guardada: …0000");

    // URL inválida en Personalizado.
    settings.select_provider(provider_index("custom"));
    settings.set_base_url("http://192.168.1.10/v1");
    REQUIRE(settings.validate().has_value());
    CHECK_THAT(*settings.validate(), Catch::Matchers::ContainsSubstring("https://"));
}

TEST_CASE("ajustes: una key que parece variable o incompleta no se guarda",
          "[proveedor][ajustes]") {
    const std::string kMessage =
        "Eso parece el nombre de una variable o una key incompleta; pega la key completa.";
    ProviderSettings settings(saved("nvidia", "https://integrate.api.nvidia.com/v1", "m"),
                              credentials_with("nvidia", kSecret), {});
    const std::string rejected[] = {
        "$NIMKEY",
        "  ",
        "abc",
        "$" + std::string(40, 'a'),                     // Larga, pero empieza con "$".
        std::string(30, 'a') + " " + std::string(5, 'b'), // Con un espacio.
        std::string(30, 'a') + "\n",                    // Con un salto de línea.
        std::string(19, 'k'),                           // Un carácter menos del mínimo.
    };
    for (const std::string& key : rejected) {
        INFO("largo " << key.size());
        settings.set_key(key);
        const std::optional<std::string> error = settings.validate();
        REQUIRE(error.has_value());
        CHECK(*error == kMessage); // Nunca incluye el valor.
        CHECK_FALSE(settings.credential_update().has_value());
    }

    const std::string accepted[] = {std::string(70, 'x'), "nvapi-" + std::string(64, 'Z'),
                                    std::string(20, 'k')};
    for (const std::string& key : accepted) {
        INFO("largo " << key.size());
        settings.set_key(key);
        CHECK_FALSE(settings.validate().has_value());
        CHECK(settings.credential_update().has_value());
    }

    // Sin key escrita se conserva la guardada: no se valida.
    settings.set_key("");
    CHECK_FALSE(settings.validate().has_value());
    // Con CHAT_API_KEY definida, la del formulario no cuenta.
    SettingsEnv env;
    env.api_key = "$EN_EL_ENTORNO";
    ProviderSettings locked(saved("nvidia", "https://integrate.api.nvidia.com/v1", "m"),
                            credentials_with("nvidia", kSecret), env);
    CHECK_FALSE(locked.set_key("abc"));
    CHECK_FALSE(locked.validate().has_value());
}

TEST_CASE("ajustes: key vacía válida en Ollama y en un host local", "[proveedor][ajustes]") {
    for (const ConfigFileValues& values : {
             saved("ollama", "http://localhost:11434/v1", "m"),
             saved("custom", "http://localhost:8080/v1", "m")}) {
        ProviderSettings settings(values, {}, {});
        CHECK_FALSE(settings.validate().has_value());
        CHECK_FALSE(settings.credential_update().has_value());
    }
}

TEST_CASE("ajustes: aviso de URL base sin ruta, sin bloquear", "[proveedor][ajustes]") {
    for (const char* url : {"https://h", "https://h/", "https://h:8443", "http://localhost:8080"}) {
        INFO(url);
        ProviderSettings settings(saved("custom", url, "m"),
                                  credentials_with(chatbot::credentials_key("custom", url), kSecret),
                                  {});
        const std::optional<std::string> hint = settings.base_url_hint();
        REQUIRE(hint.has_value());
        CHECK_THAT(*hint, Catch::Matchers::ContainsSubstring("/v1"));
        // Solo es un aviso: se puede guardar y pedir modelos.
        CHECK(settings.validate() == std::nullopt);
        CHECK(settings.can_request_models());
    }
    for (const char* url :
         {"https://h/v1", "https://h:8443/openai/v1", "http://h", "no es url", ""}) {
        INFO(url);
        ProviderSettings settings(saved("custom", url, "m"), {}, {});
        CHECK_FALSE(settings.base_url_hint().has_value());
    }

    // Se actualiza al escribir.
    ProviderSettings typing(saved("custom", "https://h/v1", "m"), {}, {});
    CHECK_FALSE(typing.base_url_hint().has_value());
    REQUIRE(typing.set_base_url("https://h"));
    CHECK(typing.base_url_hint().has_value());

    // Un proveedor conocido nunca, aunque CHAT_BASE_URL no tenga ruta.
    for (std::size_t i = 0; i < chatbot::cli::providers().size(); ++i) {
        if (chatbot::cli::providers()[i].id == chatbot::cli::kCustomProvider) {
            continue;
        }
        ProviderSettings settings(saved("custom", "https://h", "m"), {}, {});
        settings.select_provider(i);
        CHECK_FALSE(settings.base_url_hint().has_value());
        SettingsEnv env;
        env.base_url = "https://h";
        ProviderSettings locked(saved(std::string{chatbot::cli::providers()[i].id}, "", "m"), {},
                                env);
        CHECK_FALSE(locked.base_url_hint().has_value());
    }

    // En "custom" cuenta la URL efectiva: CHAT_BASE_URL.
    SettingsEnv env;
    env.base_url = "https://h";
    ProviderSettings locked(saved("custom", "https://h/v1", "m"), {}, env);
    CHECK(locked.base_url_hint().has_value());
}
