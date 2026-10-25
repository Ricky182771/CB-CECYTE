#ifndef CHATBOT_BOT_SESSIONS_H
#define CHATBOT_BOT_SESSIONS_H

#include "chatbot/bot/types.h"

#include "chatbot/types.h"

#include <chrono>
#include <cstddef>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace chatbot::bot {

using TimePoint = std::chrono::system_clock::time_point;

/// La conversación de un chat, solo en memoria. El historial empieza con el
/// mensaje de sistema (si hay instrucciones) y después alterna User y
/// Assistant: solo crece de par en par, con commit(), así que nunca pierde
/// ni duplica el mensaje de sistema ni termina en un User sin respuesta.
class Session {
public:
    Session(std::string_view system_prompt, TimePoint now);

    [[nodiscard]] const std::vector<Message>& history() const { return history_; }

    /// Lo que se manda al modelo: el historial y un User con user_content.
    [[nodiscard]] std::vector<Message> request(std::string user_content) const;

    /// La respuesta terminó bien: agrega el par al historial.
    void commit(std::string user_content, std::string answer);

    /// true desde que se acepta un mensaje hasta que se mandó su respuesta
    /// (o el error): mientras, los mensajes del chat reciben un aviso.
    bool busy = false;
    TimePoint last_activity;
    /// Quien escribe en el chat (en un chat privado, siempre el mismo).
    UserId user = 0;
    /// Perdió el acceso con la respuesta en curso: Bot borra la sesión al
    /// liberarla, sin guardar ese par.
    bool revoked = false;

private:
    std::vector<Message> history_;
};

/// Una Session por chat. Una sesión expira tras idle_timeout sin actividad,
/// salvo si está ocupada. No es segura entre hilos (Bot la protege).
class SessionStore {
public:
    SessionStore(std::string system_prompt, std::chrono::minutes idle_timeout);

    /// La sesión del chat: la existente o una nueva si no había o si la que
    /// había expiró. No cambia last_activity.
    Session& get(ChatId chat, TimePoint now);

    /// La sesión del chat si existe (aunque haya expirado), o nullptr.
    [[nodiscard]] Session* find(ChatId chat);

    /// /nuevo: borra la sesión del chat (el siguiente mensaje empieza otra,
    /// solo con el mensaje de sistema). No toca una sesión ocupada: devuelve
    /// false.
    bool reset(ChatId chat);

    /// Borra la sesión del chat aunque esté ocupada.
    void erase(ChatId chat) { sessions_.erase(chat); }

    /// Borra las sesiones que expiraron y no están ocupadas. Devuelve
    /// cuántas borró.
    std::size_t expire(TimePoint now);

    [[nodiscard]] std::size_t size() const { return sessions_.size(); }

    /// Para las sesiones nuevas; las que ya existen se quedan con las suyas.
    void set_system_prompt(std::string system_prompt) { system_prompt_ = std::move(system_prompt); }
    /// También para las que ya existen.
    void set_idle_timeout(std::chrono::minutes idle_timeout) { idle_timeout_ = idle_timeout; }

    /// Quien ya no tiene acceso (allowed(user) da false): borra sus sesiones
    /// libres y marca las ocupadas (revoked). Quita la marca a quien lo
    /// recuperó. Devuelve cuántas borró.
    std::size_t revoke(const std::function<bool(UserId)>& allowed);

private:
    [[nodiscard]] bool expired(const Session& session, TimePoint now) const;

    std::string system_prompt_;
    std::chrono::minutes idle_timeout_;
    std::map<ChatId, Session> sessions_;
};

/// Cuántos mensajes manda cada usuario al modelo en el día. No es segura
/// entre hilos (Bot la protege).
class DailyCounter {
public:
    explicit DailyCounter(std::size_t limit) : limit_(limit) {}

    /// Cuenta un mensaje de user en day ("AAAA-MM-DD", la fecha local). Si
    /// ya llegó al tope de ese día, no cuenta y devuelve false. Al cambiar
    /// de día, los conteos de días anteriores se borran.
    bool try_count(UserId user, std::string_view day);

    /// Devuelve un mensaje que se contó en day y al final no llegó al modelo
    /// (/buscar sin resultados o con error). Si ya cambió de día, nada.
    void refund(UserId user, std::string_view day);

    [[nodiscard]] std::size_t limit() const { return limit_; }
    /// Nuevo tope; los conteos del día se conservan.
    void set_limit(std::size_t limit) { limit_ = limit; }

private:
    std::size_t limit_;
    std::string day_;
    std::map<UserId, std::size_t> counts_;
};

} // namespace chatbot::bot

#endif // CHATBOT_BOT_SESSIONS_H
