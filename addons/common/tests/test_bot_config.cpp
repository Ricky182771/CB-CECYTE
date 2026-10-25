#include "chatbot/bot/config.h"

#include "chatbot/error.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>

namespace {

using chatbot::bot::BotConfig;
using chatbot::bot::validate_bot_config;

/// El mensaje del error de validación, o "" si la configuración es válida.
std::string problem(BotConfig config) {
    const auto result = validate_bot_config(std::move(config));
    if (result.is_ok()) {
        return {};
    }
    CHECK(result.error().kind == chatbot::ErrorKind::Config);
    return result.error().message;
}

} // namespace

TEST_CASE("bot config: los valores predeterminados son válidos y cierran el acceso",
          "[bot][config]") {
    const BotConfig config;
    CHECK(config.allowed_users.empty());
    CHECK(config.invite_code.empty());
    CHECK(config.system_prompt == chatbot::kDefaultSystemPrompt);
    CHECK(config.session_idle_timeout == std::chrono::minutes{60});
    CHECK(config.max_concurrent_responses == 4);
    CHECK(problem(config).empty());
}

TEST_CASE("bot config: IDs de usuario", "[bot][config]") {
    BotConfig config;
    config.allowed_users = {1, 987654321};
    CHECK(problem(config).empty());
    config.allowed_users = {5, 0};
    CHECK(problem(config).find("mayores que 0") != std::string::npos);
    config.allowed_users = {-100};
    CHECK_FALSE(problem(config).empty());
}

TEST_CASE("bot config: código de invitación", "[bot][config]") {
    BotConfig config;
    config.invite_code = "Abc_123-xyz";
    CHECK(problem(config).empty());
    config.invite_code = std::string(64, 'a');
    CHECK(problem(config).empty());

    config.invite_code = "corto12";
    CHECK(problem(config).find("de 8 a 64") != std::string::npos);
    config.invite_code = std::string(65, 'a');
    CHECK_FALSE(problem(config).empty());
    for (const char* bad : {"con espacio", "acentuado-ñ", "punto.final", "barra/123"}) {
        config.invite_code = bad;
        const std::string message = problem(config);
        INFO(bad);
        CHECK_FALSE(message.empty());
        // El motivo nunca repite el código.
        CHECK(message.find(bad) == std::string::npos);
    }
}

TEST_CASE("bot config: tiempos y topes fuera de rango", "[bot][config]") {
    BotConfig config;
    config.session_idle_timeout = std::chrono::minutes{0};
    CHECK(problem(config).find("minutos") != std::string::npos);
    config.session_idle_timeout = std::chrono::minutes{24 * 60 + 1};
    CHECK_FALSE(problem(config).empty());
    config.session_idle_timeout = std::chrono::minutes{24 * 60};
    CHECK(problem(config).empty());

    config.daily_message_limit = 0;
    CHECK(problem(config).find("por día") != std::string::npos);
    config.daily_message_limit = chatbot::addon_settings::kMaxDailyMessages + 1;
    CHECK_FALSE(problem(config).empty());
    config.daily_message_limit = 1;
    CHECK(problem(config).empty());

    config.max_concurrent_responses = 0;
    CHECK(problem(config).find("simultáneas") != std::string::npos);
    config.max_concurrent_responses = chatbot::addon_settings::kMaxConcurrentResponses + 1;
    CHECK_FALSE(problem(config).empty());
}

TEST_CASE("bot config: instrucciones del sistema", "[bot][config]") {
    BotConfig config;
    config.system_prompt = "";
    CHECK(problem(config).empty()); // Sin mensaje de sistema.
    config.system_prompt = std::string(chatbot::kMaxSystemPromptBytes + 1, 'a');
    CHECK(problem(config).find("Instrucciones del sistema") != std::string::npos);
    config.system_prompt = "con \x1B escape";
    CHECK_FALSE(problem(config).empty());
}
