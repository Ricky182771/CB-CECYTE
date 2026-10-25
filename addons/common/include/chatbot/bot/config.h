#ifndef CHATBOT_BOT_CONFIG_H
#define CHATBOT_BOT_CONFIG_H

#include "chatbot/bot/types.h"

#include "chatbot/addon_settings/limits.h"

#include "chatbot/result.h"
#include "chatbot/system_prompt.h"

#include <chrono>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace chatbot::bot {

/// Todo lo que decide el comportamiento del bot. Los valores predeterminados
/// dejan el acceso cerrado: sin allowed_users ni invite_code nadie entra.
/// Leerla de un archivo o del entorno es de cada addon.
struct BotConfig {
    /// Usuarios que pueden usar el bot (IDs > 0).
    std::vector<UserId> allowed_users;
    /// /start <código> autoriza a quien lo mande (vacío: sin código). Bot
    /// los guarda con BotDependencies::save_invited (si no hay, solo en
    /// memoria).
    std::string invite_code;
    /// Instrucciones del sistema ya resueltas (como resolve_system_prompt en
    /// la app); vacías: sin mensaje de sistema.
    std::string system_prompt{kDefaultSystemPrompt};
    /// Una conversación sin mensajes durante este tiempo se borra.
    std::chrono::minutes session_idle_timeout{60};
    /// Mensajes que cada usuario puede mandar al modelo por día (fecha local).
    std::size_t daily_message_limit = 50;
    /// Respuestas que se preparan a la vez en todo el bot; las demás esperan
    /// su turno en orden de llegada.
    std::size_t max_concurrent_responses = 4;
    /// Límite del historial que se envía (trim_history; 0 = sin límite).
    std::size_t history_limit_bytes = 32000;
    /// Un mensaje que el usuario mandó hace más que esto (llegó tarde: el
    /// bot estaba apagado o sin red) no va al modelo ni cuenta para el tope:
    /// recibe kStaleMessage. Los comandos que no llaman al modelo sí se
    /// atienden.
    std::chrono::minutes stale_after{10};
    /// Cada cuánto se actualiza la vista previa de una respuesta, como mucho.
    std::chrono::milliseconds draft_interval{1000};
    /// true: el registro de un error lleva el mensaje del servidor
    /// (detalle=…), que puede repetir texto del usuario. Solo para
    /// diagnosticar.
    bool log_error_details = false;
};

/// Configuración que ya pasó validate_bot_config: la única que acepta Bot.
class ValidBotConfig {
public:
    [[nodiscard]] const BotConfig& get() const { return config_; }

private:
    friend Result<ValidBotConfig> validate_bot_config(BotConfig config);
    explicit ValidBotConfig(BotConfig config) : config_(std::move(config)) {}

    BotConfig config_;
};

/// Revisa cada campo con los validadores de chatbot/addon_settings/limits.h
/// (los mismos que usa la pantalla de configuración de la app) y
/// system_prompt con validate_system_prompt.
/// Fuera de rango: error Config con el motivo en español (nunca el código).
[[nodiscard]] Result<ValidBotConfig> validate_bot_config(BotConfig config);

} // namespace chatbot::bot

#endif // CHATBOT_BOT_CONFIG_H
