#include "chatbot/bot/replies.h"

#include "chatbot/search_context.h"

#include <algorithm>
#include <string>

namespace chatbot::bot {
namespace {

constexpr std::string_view kBadConfig =
    "El bot no está bien configurado. Avísale al administrador.";
constexpr std::string_view kBadSearchConfig =
    "La búsqueda web no está bien configurada. Avísale al administrador.";

/// "1 minuto", "45 minutos", "1 hora", "2 horas" (horas solo si son exactas).
std::string duration_text(std::chrono::minutes minutes) {
    const auto count = minutes.count();
    if (count >= 60 && count % 60 == 0) {
        const auto hours = count / 60;
        return std::to_string(hours) + (hours == 1 ? " hora" : " horas");
    }
    return std::to_string(count) + (count == 1 ? " minuto" : " minutos");
}

std::string model_error(const ChatError& error) {
    switch (error.kind) {
    case ErrorKind::Config:
    case ErrorKind::Auth:
    case ErrorKind::ModelNotFound:
        return std::string{kBadConfig};
    case ErrorKind::RateLimited:
        return "El servicio de IA recibió demasiadas peticiones. Intenta de nuevo en unos minutos.";
    case ErrorKind::Server:
        return "El servicio de IA tiene problemas en este momento. Intenta más tarde.";
    case ErrorKind::Network:
        return "No me pude conectar con el servicio de IA. Intenta de nuevo en un momento.";
    case ErrorKind::Timeout:
        return "El servicio de IA tardó demasiado en responder. Intenta de nuevo.";
    case ErrorKind::BadResponse:
        // Una redirección (3xx) es una URL base equivocada: configuración.
        if (error.http_status >= 300 && error.http_status <= 399) {
            return std::string{kBadConfig};
        }
        return "El servicio de IA respondió algo que no entendí. Intenta de nuevo.";
    case ErrorKind::InvalidRequest:
        return "La conversación es demasiado larga para el modelo o el mensaje no es válido. "
               "Escribe /nuevo para empezar otra.";
    case ErrorKind::Cancelled:
        return "Se canceló la respuesta.";
    case ErrorKind::NoSearchResults:
        return std::string{kNoSearchResults};
    }
    return "Algo salió mal. Intenta de nuevo.";
}

std::string search_error(const ChatError& error) {
    switch (error.kind) {
    case ErrorKind::Config:
    case ErrorKind::Auth:
    case ErrorKind::ModelNotFound:
        return std::string{kBadSearchConfig};
    case ErrorKind::RateLimited:
        return "Se agotaron las búsquedas disponibles por ahora. Intenta más tarde.";
    case ErrorKind::Server:
        return "El servicio de búsqueda tiene problemas en este momento. Intenta más tarde.";
    case ErrorKind::Network:
        return "No me pude conectar con el servicio de búsqueda. Intenta de nuevo en un momento.";
    case ErrorKind::Timeout:
        return "La búsqueda tardó demasiado. Intenta de nuevo.";
    case ErrorKind::BadResponse:
        if (error.http_status >= 300 && error.http_status <= 399) {
            return std::string{kBadSearchConfig};
        }
        return "El servicio de búsqueda respondió algo que no entendí. Intenta de nuevo.";
    case ErrorKind::InvalidRequest:
        return "La búsqueda no es válida. Prueba con otra consulta.";
    case ErrorKind::Cancelled:
        return "Se canceló la búsqueda.";
    case ErrorKind::NoSearchResults:
        return std::string{kNoSearchResults};
    }
    return "Algo salió mal con la búsqueda. Intenta de nuevo.";
}

} // namespace

std::string access_denied_text(UserId user) {
    return std::string{kAccessDenied} + "\nTu ID es " + std::to_string(user) +
           ". Si te van a dar acceso, dáselo a tu profe.";
}

std::string daily_limit_reached(std::size_t limit) {
    return "Llegaste al límite de " + std::to_string(limit) +
           (limit == 1 ? " mensaje" : " mensajes") + " por hoy. Vuelve mañana.";
}

std::string help_text(bool search_available, std::size_t daily_limit,
                      std::chrono::minutes idle_timeout) {
    std::string text = "Escríbeme tu pregunta y te respondo.\n\nComandos:\n";
    text += "/nuevo: empieza una conversación nueva\n";
    if (search_available) {
        text += "/buscar <consulta>: busca en la web y responde con las fuentes\n";
    }
    text += "/ayuda: muestra esta ayuda\n\n";
    text += "Puedes mandar hasta " + std::to_string(daily_limit) +
            (daily_limit == 1 ? " mensaje" : " mensajes") +
            " al día. La conversación se borra sola tras " + duration_text(idle_timeout) +
            " sin mensajes, o con /nuevo.";
    return text;
}

std::string finish_note(std::string_view finish_reason) {
    if (finish_reason.empty() || finish_reason == "stop") {
        return {};
    }
    if (finish_reason == "length") {
        return "(La respuesta se cortó por el límite de tokens del modelo.)";
    }
    if (finish_reason == "content_filter") {
        return "(El filtro de contenido del proveedor detuvo la respuesta.)";
    }
    // El valor viene del servidor: solo se repite si es una palabra corta.
    const bool word = finish_reason.size() <= 32 &&
                      std::all_of(finish_reason.begin(), finish_reason.end(),
                                  [](char c) { return (c >= 'a' && c <= 'z') || c == '_'; });
    return word ? "(La respuesta terminó por: " + std::string{finish_reason} + ".)"
                : std::string{"(La respuesta terminó antes de tiempo.)"};
}

std::string user_error_message(const ChatError& error, ErrorSource source) {
    return source == ErrorSource::Search ? search_error(error) : model_error(error);
}

} // namespace chatbot::bot
