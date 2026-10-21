#include "chatbot/telegram/api.h"

#include "integer.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <exception>
#include <limits>
#include <utility>

namespace chatbot::telegram {
namespace {

using nlohmann::json;

/// Tope de lo que se repite de un mensaje de Telegram o del transporte.
constexpr std::size_t kMaxDetail = 200;
/// Tope de parameters.retry_after que se respeta.
constexpr std::int64_t kMaxRetryAfter = 300;

ChatError make_error(ErrorKind kind, int status, std::string message,
                     std::optional<std::chrono::seconds> retry_after = std::nullopt) {
    return ChatError{kind, status, std::move(message), retry_after};
}

/// Una línea, sin controles y con tope (sin partir caracteres).
std::string one_line(std::string_view text) {
    std::string out;
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        out += byte < 0x20 || byte == 0x7F ? ' ' : c;
    }
    if (out.size() > kMaxDetail) {
        std::size_t end = kMaxDetail;
        while (end > 0 && (static_cast<unsigned char>(out[end]) & 0xC0) == 0x80) {
            --end;
        }
        out.resize(end);
        out += "...";
    }
    return out;
}

ErrorKind kind_for_status(int status) {
    switch (status) {
    case 400:
        return ErrorKind::InvalidRequest;
    case 401:
    case 403:
    case 404:
        return ErrorKind::Auth;
    case 409:
        return ErrorKind::Config;
    case 429:
        return ErrorKind::RateLimited;
    default:
        return status >= 500 && status <= 599 ? ErrorKind::Server : ErrorKind::BadResponse;
    }
}

const char* status_text(int status) {
    switch (status) {
    case 401:
    case 404:
        return "el token de Telegram no es válido";
    case 403:
        return "Telegram no permite mandar a ese chat (¿el usuario bloqueó al bot?)";
    case 409:
        return "otro programa está usando el mismo token (o el bot tiene un webhook)";
    case 429:
        return "demasiadas peticiones a Telegram";
    default:
        return status >= 500 ? "Telegram tiene problemas" : "Telegram rechazó la petición";
    }
}

json chat_message_json(std::int64_t chat) {
    return json{{"chat_id", chat}};
}

std::optional<std::chrono::seconds> retry_after_of(const json& body) {
    const auto parameters = body.find("parameters");
    if (parameters == body.end() || !parameters->is_object()) {
        return std::nullopt;
    }
    const auto value = parameters->find("retry_after");
    if (value == parameters->end() || !value->is_number_integer()) {
        return std::nullopt;
    }
    const std::int64_t seconds = std::clamp<std::int64_t>(value->get<std::int64_t>(), 1,
                                                          kMaxRetryAfter);
    return std::chrono::seconds{seconds};
}

/// El chat de un update: id (número o cadena) y tipo. nullopt si falta o el
/// id no es un entero.
std::optional<std::pair<std::int64_t, std::string>> parse_chat(const json& parent) {
    const auto chat = parent.find("chat");
    if (chat == parent.end() || !chat->is_object()) {
        return std::nullopt;
    }
    const auto id = chat->find("id");
    if (id == chat->end()) {
        return std::nullopt;
    }
    const std::optional<std::int64_t> value = telegram_integer(*id);
    if (!value.has_value()) {
        return std::nullopt;
    }
    std::string type;
    if (const auto found = chat->find("type"); found != chat->end() && found->is_string()) {
        type = found->get<std::string>();
    }
    return std::pair{*value, std::move(type)};
}

/// Un Message. Si no se puede interpretar, nullopt y el motivo en reason.
std::optional<ChatMessage> parse_message(const json& message, std::string& reason) {
    if (!message.is_object()) {
        reason = "no_es_objeto";
        return std::nullopt;
    }
    auto chat = parse_chat(message);
    if (!chat.has_value()) {
        reason = "chat_invalido";
        return std::nullopt;
    }
    ChatMessage out;
    out.chat_id = chat->first;
    out.chat_type = std::move(chat->second);
    if (const auto from = message.find("from"); from != message.end()) {
        // Sin from (canales) el mensaje se queda sin remitente; con un from
        // que no se entiende, no se adivina.
        std::optional<std::int64_t> value;
        if (from->is_object()) {
            if (const auto id = from->find("id"); id != from->end()) {
                value = telegram_integer(*id);
            }
        }
        if (!value.has_value()) {
            reason = "from_invalido";
            return std::nullopt;
        }
        out.from_id = *value;
        if (const auto bot = from->find("is_bot"); bot != from->end() && bot->is_boolean()) {
            out.from_bot = bot->get<bool>();
        }
    }
    if (const auto text = message.find("text"); text != message.end() && text->is_string()) {
        out.text = text->get<std::string>();
    }
    if (const auto date = message.find("date"); date != message.end()) {
        const std::optional<std::int64_t> value = telegram_integer(*date);
        if (!value.has_value()) {
            reason = "date_invalido";
            return std::nullopt;
        }
        out.date = *value;
    }
    return out;
}

/// Un MessageGenerationStopped. draft_id llega como cadena (aunque la
/// documentación diga Integer) y nunca es 0 ni negativo.
std::optional<GenerationStopped> parse_stop(const json& stop, std::string& reason) {
    if (!stop.is_object()) {
        reason = "no_es_objeto";
        return std::nullopt;
    }
    auto chat = parse_chat(stop);
    if (!chat.has_value()) {
        reason = "chat_invalido";
        return std::nullopt;
    }
    const auto draft = stop.find("draft_id");
    const std::optional<std::int64_t> draft_id =
        draft != stop.end() ? telegram_integer(*draft) : std::nullopt;
    if (!draft_id.has_value() || *draft_id <= 0) {
        reason = "draft_id_invalido";
        return std::nullopt;
    }
    GenerationStopped out;
    out.chat_id = chat->first;
    out.chat_type = std::move(chat->second);
    out.draft_id = *draft_id;
    return out;
}

std::optional<ChatError> as_status(const Result<std::string>& result) {
    if (result.is_error()) {
        return result.error();
    }
    return std::nullopt;
}

} // namespace

bool plausible_token(std::string_view token) {
    const std::size_t colon = token.find(':');
    if (colon == 0 || colon == std::string_view::npos || colon > 20) {
        return false;
    }
    for (std::size_t i = 0; i < colon; ++i) {
        if (token[i] < '0' || token[i] > '9') {
            return false;
        }
    }
    const std::string_view secret = token.substr(colon + 1);
    if (secret.size() < 20 || secret.size() > 100) {
        return false;
    }
    return std::all_of(secret.begin(), secret.end(), [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
               c == '_' || c == '-';
    });
}

BotApi::BotApi(std::string token, TransportFactory transports, std::string base)
    : token_(std::move(token)), transports_(std::move(transports)), base_(std::move(base)) {}

std::string BotApi::redact(std::string_view text) const {
    std::string out{text};
    if (token_.empty()) {
        return out;
    }
    // El token tal cual, con ':' codificado (%3A) y la parte secreta sola:
    // cualquiera de ellas en un mensaje de curl o de Telegram se tapa.
    std::vector<std::string> forms{token_};
    if (const std::size_t colon = token_.find(':'); colon != std::string::npos) {
        forms.push_back(token_.substr(0, colon) + "%3A" + token_.substr(colon + 1));
        forms.push_back(token_.substr(0, colon) + "%3a" + token_.substr(colon + 1));
        if (token_.size() - colon - 1 >= 8) {
            forms.push_back(token_.substr(colon + 1));
        }
    }
    for (const std::string& form : forms) {
        for (std::size_t at = out.find(form); at != std::string::npos; at = out.find(form, at)) {
            out.replace(at, form.size(), "<token>");
            at += 7;
        }
    }
    return out;
}

std::unique_ptr<Transport> BotApi::acquire() {
    {
        const std::lock_guard<std::mutex> lock(pool_mutex_);
        if (!pool_.empty()) {
            std::unique_ptr<Transport> transport = std::move(pool_.back());
            pool_.pop_back();
            return transport;
        }
    }
    return transports_ ? transports_() : nullptr;
}

void BotApi::release(std::unique_ptr<Transport> transport) {
    if (transport == nullptr) {
        return;
    }
    const std::lock_guard<std::mutex> lock(pool_mutex_);
    if (pool_.size() < 8) {
        pool_.push_back(std::move(transport));
    }
}

Result<std::string> BotApi::call(std::string_view method, const std::string& body,
                                 std::chrono::milliseconds timeout, const CancelToken* cancel) {
    try {
        if (cancel != nullptr && cancel->is_cancelled()) {
            return make_error(ErrorKind::Cancelled, 0, "cancelado");
        }
        std::unique_ptr<Transport> transport = acquire();
        if (transport == nullptr) {
            return make_error(ErrorKind::Config, 0, "no se pudo crear el transporte HTTP");
        }
        HttpRequest request;
        request.url = base_ + "/bot" + token_ + "/" + std::string{method};
        request.body = body;
        request.timeout = timeout;
        request.cancel = cancel;
        const HttpResponse response = transport->send(request);
        release(std::move(transport));

        const std::string where = "Telegram (" + std::string{method} + "): ";
        if (response.status == 0) {
            if (response.cancelled || (cancel != nullptr && cancel->is_cancelled())) {
                return make_error(ErrorKind::Cancelled, 0, "cancelado");
            }
            const std::string detail =
                response.error.empty() ? std::string{"sin respuesta"} : one_line(redact(response.error));
            return make_error(response.timed_out ? ErrorKind::Timeout : ErrorKind::Network, 0,
                              where + detail);
        }
        if (response.status >= 300 && response.status <= 399) {
            // La URL de la redirección lleva el token si es la misma ruta:
            // no se usa.
            return make_error(ErrorKind::BadResponse, response.status,
                              where + "respondió con una redirección inesperada (HTTP " +
                                  std::to_string(response.status) + ")");
        }
        json parsed;
        try {
            parsed = json::parse(response.body);
        } catch (const json::exception&) {
            if (response.status >= 200 && response.status <= 299) {
                return make_error(ErrorKind::BadResponse, response.status,
                                  where + "respuesta que no es JSON");
            }
            return make_error(kind_for_status(response.status), response.status,
                              where + status_text(response.status));
        }
        const bool ok = parsed.is_object() && parsed.value("ok", false) == true;
        if (ok && response.status >= 200 && response.status <= 299) {
            const auto result = parsed.find("result");
            if (result == parsed.end()) {
                return make_error(ErrorKind::BadResponse, response.status,
                                  where + "respuesta sin \"result\"");
            }
            return result->dump();
        }
        int code = response.status;
        if (parsed.is_object()) {
            if (const auto value = parsed.find("error_code");
                value != parsed.end() && value->is_number_integer()) {
                code = value->get<int>();
            }
        }
        if (code >= 200 && code <= 299) {
            code = response.status >= 300 ? response.status : 400; // ok: false con 200.
        }
        std::string detail = status_text(code);
        if (parsed.is_object()) {
            if (const auto description = parsed.find("description");
                description != parsed.end() && description->is_string()) {
                detail = one_line(redact(description->get<std::string>()));
            }
        }
        return make_error(kind_for_status(code), code, where + detail,
                          parsed.is_object() ? retry_after_of(parsed) : std::nullopt);
    } catch (const std::exception&) {
        return make_error(ErrorKind::BadResponse, 0,
                          "Telegram (" + std::string{method} + "): error inesperado");
    }
}

Result<BotIdentity> BotApi::get_me(const CancelToken* cancel) {
    const Result<std::string> result = call("getMe", "{}", kRequestTimeout, cancel);
    if (result.is_error()) {
        return result.error();
    }
    try {
        const json user = json::parse(result.value());
        if (!user.is_object() || !user.contains("id") || !user["id"].is_number_integer()) {
            return make_error(ErrorKind::BadResponse, 200, "Telegram (getMe): respuesta sin id");
        }
        BotIdentity identity;
        identity.id = user["id"].get<std::int64_t>();
        if (const auto name = user.find("username"); name != user.end() && name->is_string()) {
            identity.username = one_line(name->get<std::string>());
        }
        return identity;
    } catch (const std::exception&) {
        return make_error(ErrorKind::BadResponse, 200, "Telegram (getMe): respuesta inválida");
    }
}

Result<std::vector<Update>> BotApi::get_updates(std::int64_t offset, std::chrono::seconds timeout,
                                                const CancelToken* cancel) {
    json body{{"timeout", timeout.count()},
              {"allowed_updates", json::array({"message", "stopped_message_generation"})}};
    if (offset > 0) {
        body["offset"] = offset;
    }
    const Result<std::string> result =
        call("getUpdates", body.dump(), timeout + std::chrono::seconds{15}, cancel);
    if (result.is_error()) {
        return result.error();
    }
    try {
        const json updates = json::parse(result.value());
        if (!updates.is_array()) {
            return make_error(ErrorKind::BadResponse, 200,
                              "Telegram (getUpdates): \"result\" no es una lista");
        }
        std::vector<Update> out;
        for (const json& item : updates) {
            Update update;
            if (!item.is_object()) {
                update.problem = UpdateProblem{"otro", "no_es_objeto"};
                out.push_back(std::move(update));
                continue;
            }
            const auto message = item.find("message");
            const auto stop = item.find("stopped_message_generation");
            const char* type = stop != item.end()      ? "stopped_message_generation"
                               : message != item.end() ? "message"
                                                       : "otro";
            const auto id = item.find("update_id");
            const std::optional<std::int64_t> update_id =
                id != item.end() ? telegram_integer(*id) : std::nullopt;
            if (!update_id.has_value() || *update_id <= 0 ||
                *update_id == std::numeric_limits<std::int64_t>::max()) {
                // Sin update_id no se puede confirmar ni atender: solo se
                // anota (los siguientes lo confirman al subir el offset).
                // El máximo tampoco: el offset es update_id + 1 y desbordaría.
                update.problem = UpdateProblem{type, "update_id_invalido"};
                out.push_back(std::move(update));
                continue;
            }
            update.update_id = *update_id;
            std::string reason;
            if (message != item.end()) {
                update.message = parse_message(*message, reason);
                if (!update.message.has_value()) {
                    update.problem = UpdateProblem{"message", reason};
                }
            }
            if (stop != item.end()) {
                update.stop = parse_stop(*stop, reason);
                if (!update.stop.has_value()) {
                    update.problem = UpdateProblem{"stopped_message_generation", reason};
                }
            }
            out.push_back(std::move(update));
        }
        return out;
    } catch (const std::exception&) {
        return make_error(ErrorKind::BadResponse, 200, "Telegram (getUpdates): respuesta inválida");
    }
}

std::optional<ChatError> BotApi::send_message(std::int64_t chat, std::string_view text,
                                              const CancelToken* cancel) {
    json body = chat_message_json(chat);
    body["text"] = std::string{text};
    body["link_preview_options"] = json{{"is_disabled", true}};
    return as_status(call("sendMessage", body.dump(-1, ' ', false, json::error_handler_t::replace),
                          kRequestTimeout, cancel));
}

std::optional<ChatError> BotApi::send_rich_message(std::int64_t chat, std::string_view markdown,
                                                   const CancelToken* cancel) {
    json body = chat_message_json(chat);
    body["rich_message"] = json{{"markdown", std::string{markdown}}, {"skip_entity_detection", true}};
    return as_status(call("sendRichMessage",
                          body.dump(-1, ' ', false, json::error_handler_t::replace),
                          kRequestTimeout, cancel));
}

std::optional<ChatError> BotApi::send_rich_message_draft(std::int64_t chat, std::int64_t draft_id,
                                                         std::string_view markdown,
                                                         const CancelToken* cancel) {
    json body = chat_message_json(chat);
    body["draft_id"] = draft_id;
    body["rich_message"] = json{{"markdown", std::string{markdown}}, {"skip_entity_detection", true}};
    body["can_stop"] = true;
    body["keep_on_stop"] = true;
    return as_status(call("sendRichMessageDraft",
                          body.dump(-1, ' ', false, json::error_handler_t::replace),
                          kRequestTimeout, cancel));
}

std::optional<ChatError> BotApi::send_chat_action(std::int64_t chat, const CancelToken* cancel) {
    json body = chat_message_json(chat);
    body["action"] = "typing";
    return as_status(call("sendChatAction", body.dump(), kRequestTimeout, cancel));
}

std::optional<ChatError> BotApi::set_my_commands(const std::vector<BotCommandInfo>& commands,
                                                 const CancelToken* cancel) {
    json list = json::array();
    for (const BotCommandInfo& command : commands) {
        list.push_back(json{{"command", command.command}, {"description", command.description}});
    }
    const json body{{"commands", list}};
    return as_status(call("setMyCommands", body.dump(-1, ' ', false, json::error_handler_t::replace),
                          kRequestTimeout, cancel));
}

} // namespace chatbot::telegram
