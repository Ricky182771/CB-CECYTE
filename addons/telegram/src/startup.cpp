#include "chatbot/telegram/startup.h"

#include "chatbot/platform.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>

namespace chatbot::telegram {

std::vector<BotCommandInfo> bot_commands(bool search_available) {
    std::vector<BotCommandInfo> commands{
        {"start", "Empezar (o entrar con un código de invitación)"},
        {"ayuda", "Ver qué puedo hacer"},
        {"nuevo", "Empezar una conversación nueva"},
    };
    if (search_available) {
        commands.push_back({"buscar", "Buscar en la web y responder con las fuentes"});
    }
    return commands;
}

Result<BotIdentity> connect_to_telegram(BotApi& api, bot::Log& log, const CancelToken& shutdown,
                                        Sleeper* sleeper, std::chrono::seconds max_backoff) {
    RealSleeper real_sleeper;
    Sleeper& waiter = sleeper != nullptr ? *sleeper : real_sleeper;
    for (unsigned failures = 0;; ++failures) {
        Result<BotIdentity> identity = api.get_me(&shutdown);
        if (identity.is_ok()) {
            if (failures > 0) {
                log.write(bot::LogLevel::Info,
                          "conexion_recuperada fallos=" + std::to_string(failures));
            }
            return identity;
        }
        const ChatError& error = identity.error();
        if (error.kind == ErrorKind::Cancelled || shutdown.is_cancelled()) {
            return ChatError{ErrorKind::Cancelled, 0, "cancelado", std::nullopt};
        }
        if (error.kind == ErrorKind::Auth) {
            return ChatError{ErrorKind::Auth, error.http_status,
                             "Telegram no aceptó el token (HTTP " +
                                 std::to_string(error.http_status) +
                                 "). Revisa que sea el que te dio @BotFather.",
                             std::nullopt};
        }
        if (failures == 0) {
            log.write(bot::LogLevel::Warning,
                      "sin_conexion tipo=\"" + std::string{error_kind_label(error.kind)} +
                          "\" http=" + std::to_string(error.http_status));
        }
        const std::chrono::seconds wait =
            error.kind == ErrorKind::RateLimited && error.retry_after.has_value()
                ? *error.retry_after
                : std::min(max_backoff,
                           std::chrono::seconds{std::int64_t{1} << std::min(failures, 16U)});
        if (!waiter.sleep_for(wait, &shutdown)) {
            return ChatError{ErrorKind::Cancelled, 0, "cancelado", std::nullopt};
        }
    }
}

std::optional<ChatError> prepare_data_dir(const std::string& dir) {
    const auto failure = [&dir](const std::string& cause) {
        return ChatError{ErrorKind::Config, 0,
                         "No se puede escribir en la carpeta de datos del bot: " + dir + " (" +
                             cause + ").",
                         std::nullopt};
    };
    if (std::optional<std::string> error =
            create_private_directory(dir, FolderPrivacy::PrivateIfNew)) {
        return failure(*error);
    }
    // Que exista no basta: puede ser de solo lectura.
    const std::string probe = dir + "/.prueba-escritura";
    if (std::optional<std::string> error =
            write_file_atomic(probe, "", FilePrivacy::Private, FolderPrivacy::PrivateIfNew)) {
        return failure(*error);
    }
    std::error_code ignored;
    std::filesystem::remove(probe, ignored); // Si se queda, está vacío y no estorba.
    return std::nullopt;
}

int exit_code_for(const ChatError& error, FailureSource source) {
    switch (source) {
    case FailureSource::Telegram:
        return error.kind == ErrorKind::Auth ? kExitConfig : kExitFailure;
    case FailureSource::DataDir:
        return kExitCantCreate;
    case FailureSource::Invited:
        return error.kind == ErrorKind::BadResponse ? kExitDataError : kExitFailure;
    }
    return kExitFailure;
}

} // namespace chatbot::telegram
