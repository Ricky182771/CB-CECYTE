#include "chatbot/bot/replies.h"

#include "chatbot/error.h"
#include "chatbot/search_context.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>
#include <vector>

namespace {

using chatbot::ChatError;
using chatbot::ErrorKind;
using chatbot::bot::ErrorSource;
using chatbot::bot::user_error_message;

const std::vector<ErrorKind> kAllKinds = {
    ErrorKind::Config,      ErrorKind::Auth,           ErrorKind::ModelNotFound,
    ErrorKind::RateLimited, ErrorKind::Server,         ErrorKind::Network,
    ErrorKind::Timeout,     ErrorKind::BadResponse,    ErrorKind::InvalidRequest,
    ErrorKind::Cancelled,   ErrorKind::NoSearchResults};

/// Un error que trae todo lo que nunca debe llegar al usuario.
ChatError leaky(ErrorKind kind, int status = 0) {
    return ChatError{kind, status,
                     "POST https://pruebas.invalid/v1/chat/completions con la key "
                     "clave-ficticia-de-prueba: {\"error\":{\"message\":\"cuerpo crudo\"}}",
                     std::nullopt};
}

bool contains(const std::string& text, const char* part) {
    return text.find(part) != std::string::npos;
}

} // namespace

TEST_CASE("errores al usuario: ninguno trae la key, la URL ni el cuerpo", "[bot][errores]") {
    for (const ErrorSource source : {ErrorSource::Model, ErrorSource::Search}) {
        for (const ErrorKind kind : kAllKinds) {
            for (const int status : {0, 301, 401, 500}) {
                const std::string text = user_error_message(leaky(kind, status), source);
                INFO(chatbot::error_kind_label(kind) << " " << status);
                CHECK_FALSE(text.empty());
                CHECK_FALSE(contains(text, "clave-ficticia"));
                CHECK_FALSE(contains(text, "pruebas.invalid"));
                CHECK_FALSE(contains(text, "https://"));
                CHECK_FALSE(contains(text, "cuerpo crudo"));
                CHECK_FALSE(contains(text, "{"));
            }
        }
    }
}

TEST_CASE("errores al usuario: cada tipo con su texto", "[bot][errores]") {
    const auto model = [](ErrorKind kind, int status = 0) {
        return user_error_message(leaky(kind, status), ErrorSource::Model);
    };
    for (const ErrorKind kind : {ErrorKind::Config, ErrorKind::Auth, ErrorKind::ModelNotFound}) {
        CHECK(model(kind) == "El bot no está bien configurado. Avísale al administrador.");
    }
    CHECK(contains(model(ErrorKind::RateLimited), "demasiadas peticiones"));
    CHECK(contains(model(ErrorKind::Server), "problemas"));
    CHECK(contains(model(ErrorKind::Network), "conectar"));
    CHECK(contains(model(ErrorKind::Timeout), "tardó"));
    CHECK(contains(model(ErrorKind::BadResponse), "no entendí"));
    // Una redirección es una URL base mal configurada.
    CHECK(contains(model(ErrorKind::BadResponse, 301), "administrador"));
    CHECK(contains(model(ErrorKind::InvalidRequest), "/nuevo"));
    CHECK(contains(model(ErrorKind::Cancelled), "cancel"));
    CHECK(model(ErrorKind::NoSearchResults) == chatbot::kNoSearchResults);

    const auto search = [](ErrorKind kind) {
        return user_error_message(leaky(kind), ErrorSource::Search);
    };
    CHECK(contains(search(ErrorKind::Auth), "búsqueda web no está bien configurada"));
    CHECK(contains(search(ErrorKind::RateLimited), "búsquedas"));
    CHECK(contains(search(ErrorKind::Network), "búsqueda"));
    CHECK(search(ErrorKind::NoSearchResults) == chatbot::kNoSearchResults);
}

TEST_CASE("textos: tope diario, ayuda y notas", "[bot][textos]") {
    CHECK(chatbot::bot::daily_limit_reached(50) ==
          "Llegaste al límite de 50 mensajes por hoy. Vuelve mañana.");
    CHECK(chatbot::bot::daily_limit_reached(1) ==
          "Llegaste al límite de 1 mensaje por hoy. Vuelve mañana.");

    const std::string with = chatbot::bot::help_text(true, 30, std::chrono::minutes{60});
    CHECK(contains(with, "/buscar"));
    CHECK(contains(with, "/nuevo"));
    CHECK(contains(with, "30 mensajes al día"));
    CHECK(contains(with, "1 hora"));
    const std::string without = chatbot::bot::help_text(false, 1, std::chrono::minutes{45});
    CHECK_FALSE(contains(without, "/buscar"));
    CHECK(contains(without, "1 mensaje al día"));
    CHECK(contains(without, "45 minutos"));

    CHECK(chatbot::bot::finish_note("stop").empty());
    CHECK(chatbot::bot::finish_note("").empty());
    CHECK(contains(chatbot::bot::finish_note("length"), "límite de tokens"));
    CHECK(contains(chatbot::bot::finish_note("content_filter"), "filtro"));
    CHECK(chatbot::bot::finish_note("tool_calls") == "(La respuesta terminó por: tool_calls.)");
    CHECK(chatbot::bot::finish_note("<b>raro</b>") == "(La respuesta terminó antes de tiempo.)");
}
