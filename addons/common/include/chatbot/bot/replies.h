#ifndef CHATBOT_BOT_REPLIES_H
#define CHATBOT_BOT_REPLIES_H

#include "chatbot/bot/types.h"

#include "chatbot/error.h"

#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>

namespace chatbot::bot {

// Textos que el bot manda (texto plano). Cortos, en español.

/// La frase para quien no está autorizado: no dice qué modelo usa el bot,
/// quién lo administra ni cómo se configura. Va con su ID
/// (access_denied_text).
inline constexpr std::string_view kAccessDenied = "No tienes acceso a este bot.";
inline constexpr std::string_view kWelcome =
    "¡Hola! Escríbeme tu pregunta y te respondo. Usa /ayuda para ver los comandos.";
inline constexpr std::string_view kWaitPrevious = "Espera a que termine la respuesta anterior.";
inline constexpr std::string_view kNewConversation =
    "Listo: empezamos una conversación nueva.";
inline constexpr std::string_view kSearchUsage = "Escribe qué buscar: /buscar <consulta>";
inline constexpr std::string_view kSearchUnavailable =
    "La búsqueda web no está disponible en este bot.";
inline constexpr std::string_view kTextOnly = "Por ahora solo puedo leer texto.";
/// Un mensaje que llegó tarde (BotConfig::stale_after): no va al modelo.
inline constexpr std::string_view kStaleMessage =
    "Estuve desconectado y se me pasó tu mensaje; mándalo otra vez.";
/// El usuario detuvo la respuesta: va después de lo que se alcanzó a generar.
inline constexpr std::string_view kStoppedNote = "(Detuviste la respuesta.)";
/// El usuario la detuvo antes de que llegara texto.
inline constexpr std::string_view kStoppedEmpty = "Detuviste la respuesta.";
/// Avisos de la vista previa antes del primer texto (Draft::status).
inline constexpr std::string_view kThinkingStatus = "Pensando…";
inline constexpr std::string_view kSearchingStatus = "Buscando en la web…";
inline constexpr std::string_view kEmptyAnswer = "El modelo no devolvió texto. Intenta de nuevo.";
inline constexpr std::string_view kSourcesTitle = "Fuentes:";

/// kAccessDenied y el ID del usuario, para que se lo dé a quien administra
/// el bot (y lo agregue a la lista): "No tienes acceso a este bot.\nTu ID
/// es 123. Si te van a dar acceso, dáselo a tu profe."
[[nodiscard]] std::string access_denied_text(UserId user);

/// "Llegaste al límite de 50 mensajes por hoy. Vuelve mañana."
[[nodiscard]] std::string daily_limit_reached(std::size_t limit);

/// Texto de /ayuda: los comandos (/buscar solo si hay búsqueda), el tope
/// diario y cuándo se borra la conversación.
[[nodiscard]] std::string help_text(bool search_available, std::size_t daily_limit,
                                    std::chrono::minutes idle_timeout);

/// Nota después de la respuesta según cómo terminó (finish_reason), o vacía
/// si terminó normal ("stop" o sin dato).
[[nodiscard]] std::string finish_note(std::string_view finish_reason);

/// Qué falló: el modelo o la búsqueda web.
enum class ErrorSource { Model, Search };

/// Mensaje amable para el usuario según el tipo de error. Nunca incluye
/// error.message (puede traer la URL base, el cuerpo del servidor o, en un
/// error mal armado, la key): ese detalle va solo al registro. Los errores
/// de configuración (key inválida, modelo inexistente, redirecciones) piden
/// avisarle al administrador.
[[nodiscard]] std::string user_error_message(const ChatError& error, ErrorSource source);

} // namespace chatbot::bot

#endif // CHATBOT_BOT_REPLIES_H
