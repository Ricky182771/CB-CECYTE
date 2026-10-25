#ifndef CHATBOT_BOT_ACCESS_H
#define CHATBOT_BOT_ACCESS_H

#include "chatbot/bot/config.h"
#include "chatbot/bot/types.h"

#include <cstddef>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace chatbot::bot {

/// Quién puede usar el bot: la lista de la configuración y quienes mandaron
/// el código de invitación (los de antes llegan en invited, guardados por la
/// plataforma; siguen entrando aunque después se quite el código). Acceso
/// denegado por defecto: sin lista, código ni invitados nadie entra. No es
/// segura entre hilos (Bot la protege).
class AccessControl {
public:
    explicit AccessControl(const BotConfig& config, const std::vector<UserId>& invited = {});

    [[nodiscard]] bool is_allowed(UserId user) const;

    /// /start <código>: si hay código configurado y code es exactamente ese,
    /// autoriza a user (en memoria) y devuelve true. Un código vacío o
    /// incorrecto, o sin código configurado, devuelve false sin cambiar nada.
    /// La comparación tarda lo mismo sin importar dónde difieren.
    bool redeem(UserId user, std::string_view code);

    /// Usuarios autorizados con el código (no cuenta los de la lista).
    [[nodiscard]] std::size_t invited_count() const { return invited_.size(); }

    /// Reemplaza los autorizados con el código por los guardados (la lista
    /// del archivo, que otro proceso pudo cambiar).
    void set_invited(const std::vector<UserId>& invited) {
        invited_ = {invited.begin(), invited.end()};
    }

    /// Los autorizados con el código, en orden.
    [[nodiscard]] std::vector<UserId> invited() const {
        return {invited_.begin(), invited_.end()};
    }

private:
    std::set<UserId> allowed_;
    std::set<UserId> invited_;
    std::string invite_code_;
};

} // namespace chatbot::bot

#endif // CHATBOT_BOT_ACCESS_H
