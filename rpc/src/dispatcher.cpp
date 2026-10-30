#include "dispatcher.h"

#include "block_actions.h"
#include "code_blocks.h"
#include "commands.h"
#include "providers.h"
#include "search_settings.h"
#include "sidebar.h"

#include "chatbot/addon_settings/registry.h"
#include "chatbot/addon_settings/systemd_unit.h"
#include "chatbot/system_prompt.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <limits>
#include <system_error>
#include <utility>

namespace chatbot::rpc {

namespace {

constexpr std::string_view kServerName = "chatbot-rpc";
constexpr std::string_view kEmptyMessage = "El mensaje está vacío.";
constexpr std::string_view kAlreadyInitialized = "initialize ya se llamó.";
constexpr std::string_view kAlreadyShutDown = "El servidor ya se apagó: solo queda exit.";
constexpr std::string_view kSearchKeyLocked =
    "La key de búsqueda está definida por CHAT_SEARCH_API_KEY; no se puede cambiar aquí.";
constexpr std::string_view kSearchKeyEmpty = "Escribe la key de búsqueda.";

Json optional_string(const std::optional<std::string>& value) {
    return value.has_value() ? Json(*value) : Json(nullptr);
}

Json non_empty_or_null(const std::string& value) {
    return value.empty() ? Json(nullptr) : Json(value);
}

std::string_view outcome_name(const cli::Session::Finished& finished) {
    if (!finished.error.has_value()) {
        return "ok";
    }
    switch (finished.error->kind) {
    case ErrorKind::Cancelled:
        return "cancelled";
    case ErrorKind::NoSearchResults:
        return "no_results";
    default:
        return "error";
    }
}

std::string_view reason_name(const cli::CodeBlock& block) {
    switch (block.complete ? cli::IncompleteReason::None : block.reason) {
    case cli::IncompleteReason::None:
        break;
    case cli::IncompleteReason::Cancelled:
        return "cancelled";
    case cli::IncompleteReason::Error:
        return "error";
    case cli::IncompleteReason::InProgress:
        return "in_progress";
    }
    return "none";
}

/// mode de settings.system_prompt.*: la llave ausente usa las predeterminadas.
std::string_view prompt_mode(const cli::Session& session) {
    if (!session.system_prompt_stored()) {
        return "default";
    }
    return session.system_prompt().empty() ? "none" : "custom";
}

/// El number de blocks.*, o nullopt si no cabe en un int.
std::optional<int> block_number(const Json& params) {
    const Json& number = params.at("number");
    constexpr int kMax = std::numeric_limits<int>::max();
    if (number.is_number_unsigned()) { // Los positivos llegan así.
        const auto value = number.get<std::uint64_t>();
        return value <= kMax ? std::optional<int>{static_cast<int>(value)} : std::nullopt;
    }
    const auto value = number.get<std::int64_t>();
    return value >= std::numeric_limits<int>::min() && value <= kMax
               ? std::optional<int>{static_cast<int>(value)}
               : std::nullopt;
}

constexpr std::string_view kNoHomeForService = "Sin HOME no hay dónde instalar el servicio.";

/// El nombre de cada campo en el protocolo (AddonField en snake_case).
constexpr std::array<std::string_view, cli::kAddonFieldCount> kFieldNames{
    "token",        "model",          "model_key",   "search_key",
    "allowed_users", "invite_code",   "daily_limit", "concurrent",
    "idle_minutes", "stale_minutes",  "draft_ms",    "time_zone"};

std::string_view field_name(cli::AddonField field) {
    return kFieldNames[static_cast<std::size_t>(field)];
}

Json form_json(const cli::AddonForm& form) {
    Json fields = Json::object();
    for (std::size_t i = 0; i < cli::kAddonFieldCount; ++i) {
        fields[std::string{kFieldNames[i]}] = form.text[i];
    }
    return {{"enabled", form.enabled},
            {"log_error_details", form.log_error_details},
            {"fields", std::move(fields)},
            {"remove_model_key", form.remove_model_key},
            {"remove_search_key", form.remove_search_key}};
}

/// El form de check y save, con todas sus llaves. Si falta o sobra algo, el
/// data de -32602 con "form.…" y "form.fields.…".
std::optional<Json> parse_form(const Json& json, std::optional<cli::AddonForm>& form) {
    std::optional<Json> bad = check_params(&json, {{"enabled", Param::Type::Boolean},
                                                   {"log_error_details", Param::Type::Boolean},
                                                   {"fields", Param::Type::Object},
                                                   {"remove_model_key", Param::Type::Boolean},
                                                   {"remove_search_key", Param::Type::Boolean}});
    std::string prefix = "form.";
    if (!bad.has_value()) {
        const Json& fields = json.at("fields");
        bad = check_params(&fields, {{kFieldNames[0]},
                                     {kFieldNames[1]},
                                     {kFieldNames[2]},
                                     {kFieldNames[3]},
                                     {kFieldNames[4]},
                                     {kFieldNames[5]},
                                     {kFieldNames[6]},
                                     {kFieldNames[7]},
                                     {kFieldNames[8]},
                                     {kFieldNames[9]},
                                     {kFieldNames[10]},
                                     {kFieldNames[11]}});
        prefix = "form.fields.";
    }
    if (bad.has_value()) {
        Json fields = Json::array();
        for (const Json& field : bad->at("fields")) {
            fields.push_back(prefix + field.get<std::string>());
        }
        return Json{{"fields", std::move(fields)}};
    }
    cli::AddonForm parsed;
    parsed.enabled = json.at("enabled").get<bool>();
    parsed.log_error_details = json.at("log_error_details").get<bool>();
    for (std::size_t i = 0; i < cli::kAddonFieldCount; ++i) {
        parsed.text[i] = json.at("fields").at(std::string{kFieldNames[i]}).get<std::string>();
    }
    parsed.remove_model_key = json.at("remove_model_key").get<bool>();
    parsed.remove_search_key = json.at("remove_search_key").get<bool>();
    form = std::move(parsed);
    return std::nullopt;
}

/// Un elemento de addons.list: lo que se sabe del programa en path (la
/// versión, solo si check_install o service.status ya la preguntó ahí).
Json install_json(const cli::TelegramAddon& addon, const std::optional<std::string>& path) {
    const bool portable = addon.environment().portable;
    const std::optional<cli::AddonInstall>& known = addon.install();
    std::string problem;
    Json version_ok = nullptr;
    if (!path.has_value()) {
        problem = cli::addon_install_problem(addon.info(), cli::AddonInstall{}, portable);
    } else if (known.has_value() && known->path == path) {
        problem = cli::addon_install_problem(addon.info(), *known, portable);
        version_ok = problem.empty();
    }
    return {{"name", std::string{addon.info().name}},
            {"installed", path.has_value()},
            {"path", optional_string(path)},
            {"problem", problem},
            {"config_version_ok", std::move(version_ok)}};
}

/// Por qué no se puede guardar: la sección (al leer, versión) o el addon
/// incompatible; "" si se puede.
std::string read_only_reason(const cli::TelegramAddon& addon) {
    if (std::string problem = addon.section_problem(); !problem.empty()) {
        return problem;
    }
    if (addon.read_only()) { // Instalado e incompatible.
        return cli::addon_install_problem(addon.info(), *addon.install(),
                                          addon.environment().portable);
    }
    return {};
}

std::string_view unit_state_name(addon_settings::UnitState state) {
    switch (state) {
    case addon_settings::UnitState::Missing:
        return "missing";
    case addon_settings::UnitState::Foreign:
        return "foreign";
    case addon_settings::UnitState::Current:
        return "current";
    case addon_settings::UnitState::Stale:
        return "stale";
    }
    return "missing";
}

/// service.status, con el último estado (refresh).
Json status_json(const cli::TelegramAddon& addon) {
    const cli::ServiceStatus& status = *addon.service();
    return {{"systemd", status.systemd},
            {"active", status.active},
            {"enabled", status.enabled},
            {"exit_status", status.exit_status.has_value() ? Json(*status.exit_status)
                                                           : Json(nullptr)},
            {"unit", std::string{unit_state_name(status.unit)}},
            {"linger", status.linger.has_value() ? Json(*status.linger) : Json(nullptr)},
            {"last_error", status.last_error},
            {"summary", cli::service_summary(status)},
            {"problem", cli::service_problem(status, addon.info().executable,
                                             addon.environment().data_dir.value_or(""))},
            {"mismatch", addon.out_of_sync(addon.saved_form().enabled)},
            {"journal_command", cli::journal_command(addon.info())}};
}

/// Los IDs de Telegram como cadenas decimales (sección 4).
Json user_list(const std::vector<std::int64_t>& users) {
    Json list = Json::array();
    for (const std::int64_t user : users) {
        list.push_back(std::to_string(user));
    }
    return list;
}

/// Un ID de Telegram en cadena decimal: un entero positivo de 64 bits, sin
/// signo ni espacios.
std::optional<std::int64_t> parse_user(const std::string& text) {
    std::int64_t value = 0;
    const char* end = text.data() + text.size();
    const auto [stop, error] = std::from_chars(text.data(), end, value);
    if (text.empty() || text.front() == '-' || error != std::errc{} || stop != end || value <= 0) {
        return std::nullopt;
    }
    return value;
}

} // namespace

Dispatcher::Dispatcher(ServerInfo info, SessionFactory make_session,
                       cli::ModelsLoader::TransportFactory models_transport,
                       AddonsFactory make_addons, cli::Session::Post post, Write write, Now now)
    : info_(std::move(info)), make_session_(std::move(make_session)),
      models_transport_(std::move(models_transport)), make_addons_(std::move(make_addons)),
      post_(std::move(post)),
      write_(std::move(write)), now_(std::move(now)) {}

Dispatcher::~Dispatcher() { close_session(); }

void Dispatcher::on_line(std::string_view line) {
    // Lo pendiente sale antes que cualquier respuesta.
    sync(true);
    const Json message = Json::parse(line.begin(), line.end(), nullptr, false);
    if (message.is_discarded()) {
        write_(error_response(nullptr, RpcError::ParseError));
        return;
    }
    if (!message.is_object()) { // Incluye los lotes (arreglos).
        write_(error_response(nullptr, RpcError::InvalidRequest));
        return;
    }
    const auto id_field = message.find("id");
    const bool has_id = id_field != message.end();
    if (has_id && !id_field->is_number_integer() && !id_field->is_string()) {
        write_(error_response(nullptr, RpcError::InvalidRequest));
        return;
    }
    const Json id = has_id ? *id_field : Json(nullptr);
    const auto version = message.find("jsonrpc");
    const auto method = message.find("method");
    if (version == message.end() || *version != "2.0" || method == message.end() ||
        !method->is_string()) {
        // Sin id válido tampoco es una notificación: se responde con id null.
        write_(error_response(id, RpcError::InvalidRequest));
        return;
    }
    const auto params = message.find("params");
    try {
        dispatch(method->get<std::string>(), params != message.end() ? &*params : nullptr, id,
                 has_id);
    } catch (const std::exception&) {
        fail(id, has_id, RpcError::InternalError);
    }
    sync(false); // Los eventos que provocó, después de la respuesta.
}

void Dispatcher::on_oversized_line() {
    sync(true);
    write_(error_response(nullptr, RpcError::InvalidRequest,
                          message_data("La línea pasa de 1 MiB y se descartó.")));
}

void Dispatcher::after_task() { sync(false); }

void Dispatcher::flush_due() {
    if (pending_since_.has_value() && now_() >= *pending_since_ + kDeltaWindow) {
        sync(true);
    }
}

std::optional<Dispatcher::TimePoint> Dispatcher::next_deadline() const {
    if (!pending_since_.has_value()) {
        return std::nullopt;
    }
    return *pending_since_ + kDeltaWindow;
}

void Dispatcher::on_eof() { close_session(); }

void Dispatcher::dispatch(const std::string& method, const Json* params, const Json& id,
                          bool has_id) {
    if (method == "exit") {
        // main cierra la sesión (on_eof) y sale con este código.
        exit_code_ = state_ == State::ShutDown ? 0 : 1;
        reply(id, has_id, Json::object());
        return;
    }
    if (state_ == State::ShutDown) {
        fail(id, has_id, RpcError::InvalidRequest, message_data(kAlreadyShutDown));
        return;
    }
    if (method == "shutdown") {
        if (const auto bad = check_params(params, {})) {
            fail(id, has_id, RpcError::InvalidParams, *bad);
            return;
        }
        shutdown(id, has_id);
        return;
    }
    if (method == "$/cancel") {
        // Solo las largas quedan pendientes; con otro id no hace nada.
        if (const auto bad = check_params(params, {{"id", Param::Type::IntegerOrString}})) {
            fail(id, has_id, RpcError::InvalidParams, *bad);
            return;
        }
        const Json& target = params->at("id");
        std::vector<std::uint64_t> keys;
        for (const Pending& pending : pending_) {
            if (pending.id == target) {
                keys.push_back(pending.key);
            }
        }
        for (const std::uint64_t key : keys) {
            if (models_key_ == key) {
                models_->cancel();
                models_key_.reset();
            }
            // Las de addons siguen hasta terminar (cortar systemctl a la mitad
            // es peor); su resultado ya no se responde.
            (void)untrack(key);
            fail(target, true, RpcError::RequestCancelled);
        }
        reply(id, has_id, Json::object());
        return;
    }
    if (method == "initialize") {
        initialize(params, id, has_id);
        return;
    }
    if (state_ != State::Running) {
        fail(id, has_id, RpcError::NotInitialized);
        return;
    }
    if (method == "app.status") {
        if (const auto bad = check_params(params, {})) {
            fail(id, has_id, RpcError::InvalidParams, *bad);
            return;
        }
        reply(id, has_id, status());
    } else if (method == "conversation.new") {
        if (const auto bad = check_params(params, {})) {
            fail(id, has_id, RpcError::InvalidParams, *bad);
            return;
        }
        if (const std::optional<std::string> error = session_->new_conversation()) {
            fail(id, has_id, RpcError::Busy, message_data(*error)); // kWaitOrCancel
            return;
        }
        write_(notification("conversation.closed",
                            {{"conversation", handle_}, {"reason", "replaced"}}));
        start_conversation();
        reply(id, has_id, snapshot());
    } else if (method == "conversation.get") {
        if (const auto bad = check_params(params, {{"conversation"}})) {
            fail(id, has_id, RpcError::InvalidParams, *bad);
            return;
        }
        if (check_handle(*params, id, has_id)) {
            reply(id, has_id, snapshot());
        }
    } else if (method == "chat.send") {
        if (const auto bad = check_params(
                params, {{"conversation"}, {"text"}, {"search", Param::Type::String, false}})) {
            fail(id, has_id, RpcError::InvalidParams, *bad);
            return;
        }
        if (!check_handle(*params, id, has_id)) {
            return;
        }
        const auto search = params->find("search");
        const cli::SendResult sent =
            session_->send(params->at("text").get<std::string>(),
                           search != params->end()
                               ? std::optional<std::string>{search->get<std::string>()}
                               : std::nullopt);
        if (sent == cli::SendResult::Sent) {
            reply(id, has_id, {{"accepted", true}});
        } else {
            fail_send(sent, id, has_id, kEmptyMessage);
        }
    } else if (method == "chat.cancel") {
        if (const auto bad = check_params(params, {{"conversation"}})) {
            fail(id, has_id, RpcError::InvalidParams, *bad);
            return;
        }
        if (check_handle(*params, id, has_id)) {
            const bool cancelled = session_->busy();
            session_->cancel();
            reply(id, has_id, {{"cancelled", cancelled}});
        }
    } else if (method == "conversations.list") {
        if (const auto bad = check_params(params, {})) {
            fail(id, has_id, RpcError::InvalidParams, *bad);
            return;
        }
        reply(id, has_id, {{"items", conversation_list()}});
    } else if (method == "conversation.open" || method == "conversation.delete") {
        if (const auto bad = check_params(params, {{"id"}})) {
            fail(id, has_id, RpcError::InvalidParams, *bad);
            return;
        }
        // Antes que Session: Busy no se confunde con un error del almacén.
        if (session_->busy()) {
            fail(id, has_id, RpcError::Busy, message_data(cli::kWaitOrCancel));
            return;
        }
        const std::string target = params->at("id").get<std::string>();
        const bool is_open = !target.empty() && target == session_->conversation().id();
        if (method == "conversation.open") {
            if (is_open) {
                reply(id, has_id, snapshot()); // Ya es la abierta: sin recargar.
                return;
            }
            if (const std::optional<std::string> error = session_->open(target)) {
                fail(id, has_id, RpcError::StorageError, message_data(*error));
                return;
            }
            write_(notification("conversation.closed",
                                {{"conversation", handle_}, {"reason", "replaced"}}));
            start_conversation();
            reply(id, has_id, snapshot());
            return;
        }
        if (const std::optional<std::string> error = session_->remove(target)) {
            fail(id, has_id, RpcError::StorageError, message_data(*error));
            return;
        }
        Json replaced_by = nullptr;
        if (is_open) { // Session empezó una nueva.
            write_(notification("conversation.closed",
                                {{"conversation", handle_}, {"reason", "deleted"}}));
            start_conversation();
            replaced_by = snapshot();
        }
        reply(id, has_id, {{"replaced_by", std::move(replaced_by)}});
        write_(notification("conversations.changed", Json::object()));
    } else if (method == "conversation.export") {
        if (const auto bad = check_params(params, {{"conversation"}})) {
            fail(id, has_id, RpcError::InvalidParams, *bad);
            return;
        }
        if (check_handle(*params, id, has_id)) {
            reply_file(session_->export_to_downloads(), id, has_id);
        }
    } else if (method == "commands.list") {
        if (const auto bad = check_params(params, {{"conversation"}})) {
            fail(id, has_id, RpcError::InvalidParams, *bad);
            return;
        }
        if (check_handle(*params, id, has_id)) {
            reply(id, has_id, {{"commands", command_list()}});
        }
    } else if (method == "commands.run") {
        if (const auto bad = check_params(params, {{"conversation"}, {"text"}})) {
            fail(id, has_id, RpcError::InvalidParams, *bad);
            return;
        }
        if (check_handle(*params, id, has_id)) {
            run_command(params->at("text").get<std::string>(), id, has_id);
        }
    } else if (method == "blocks.list") {
        if (const auto bad = check_params(params, {{"conversation"}})) {
            fail(id, has_id, RpcError::InvalidParams, *bad);
            return;
        }
        if (!check_handle(*params, id, has_id)) {
            return;
        }
        Json blocks = Json::array();
        for (const cli::CodeBlock& block : session_->code_blocks().blocks()) {
            blocks.push_back({{"number", block.number},
                              {"entry", block.entry},
                              {"language", block.info},
                              {"lines", cli::count_lines(block.code)},
                              {"complete", block.complete},
                              {"reason", std::string{reason_name(block)}}});
        }
        reply(id, has_id, {{"blocks", std::move(blocks)}});
    } else if (method == "blocks.get" || method == "blocks.save") {
        const bool save = method == "blocks.save";
        if (const auto bad =
                save ? check_params(params, {{"conversation"},
                                             {"number", Param::Type::Integer},
                                             {"file_name", Param::Type::String, false}})
                     : check_params(params, {{"conversation"}, {"number", Param::Type::Integer}})) {
            fail(id, has_id, RpcError::InvalidParams, *bad);
            return;
        }
        if (!check_handle(*params, id, has_id)) {
            return;
        }
        const std::optional<int> number = block_number(*params);
        if (!number.has_value()) {
            fail(id, has_id, RpcError::InvalidParams, {{"fields", {"number"}}});
            return;
        }
        if (save) {
            reply_file(session_->save_code_block(number, params->value("file_name", "")), id,
                       has_id);
            return;
        }
        const cli::CodeBlockChoice choice =
            cli::choose_code_block(session_->code_blocks().blocks(), number);
        if (choice.block == nullptr) {
            fail(id, has_id, RpcError::ValidationFailed, message_data(choice.problem));
            return;
        }
        reply(id, has_id,
              {{"code", choice.block->code},
               {"language", choice.block->info},
               {"complete", choice.block->complete},
               {"reason", std::string{reason_name(*choice.block)}}});
    } else if (method.starts_with("settings.") || method == "models.list") {
        settings(method, params, id, has_id);
    } else if (method.starts_with("addons.") || method.starts_with("service.")) {
        addons(method, params, id, has_id);
    } else {
        fail(id, has_id, RpcError::MethodNotFound);
    }
}

void Dispatcher::settings(const std::string& method, const Json* params, const Json& id,
                          bool has_id) {
    const auto changed = [this](std::string_view section) {
        write_(notification("settings.changed", {{"section", std::string{section}}}));
    };
    // Los guardados que tocan el chat no van con una respuesta en curso
    // (F2 no abre); gui sí.
    const auto busy = [&] {
        if (!session_->busy()) {
            return false;
        }
        fail(id, has_id, RpcError::Busy, message_data(cli::kWaitOrCancel));
        return true;
    };
    if (method == "settings.provider.get") {
        if (const auto bad = check_params(params, {})) {
            fail(id, has_id, RpcError::InvalidParams, *bad);
            return;
        }
        const Result<cli::ProviderSettings> form = session_->provider_form();
        if (form.is_error()) {
            fail(id, has_id, RpcError::StorageError, message_data(form.error().message));
            return;
        }
        const cli::ProviderSettings& f = form.value();
        Json providers = Json::array();
        for (const cli::ProviderInfo& provider : cli::providers()) {
            providers.push_back({{"id", std::string{provider.id}},
                                 {"name", std::string{provider.name}},
                                 {"base_url", std::string{provider.base_url}},
                                 {"needs_key", provider.needs_key}});
        }
        reply(id, has_id,
              {{"providers", std::move(providers)},
               {"saved",
                {{"provider", f.saved().provider},
                 {"base_url", f.saved().base_url},
                 {"model", f.saved().model}}},
               {"effective",
                {{"base_url", session_->config().base_url}, {"model", session_->config().model}}},
               {"locks",
                {{"base_url", f.base_url_locked()},
                 {"model", f.model_locked()},
                 {"api_key", f.key_locked()}}},
               {"key_status", f.key_status()}});
    } else if (method == "settings.provider.check" || method == "settings.provider.save" ||
               method == "models.list") {
        if (const auto bad = check_params(params, {{"draft", Param::Type::Object}})) {
            fail(id, has_id, RpcError::InvalidParams, *bad);
            return;
        }
        const std::optional<cli::ProviderSettings> form = provider_draft(*params, id, has_id);
        if (!form.has_value()) {
            return;
        }
        if (method == "settings.provider.check") {
            reply(id, has_id,
                  {{"error", optional_string(form->validate())},
                   {"hint", optional_string(form->base_url_hint())},
                   {"can_request_models", form->can_request_models()},
                   {"dirty", form->dirty()}});
        } else if (method == "models.list") {
            models_list(*form, id, has_id);
        } else if (!busy()) {
            if (const std::optional<std::string> error = form->validate()) {
                fail(id, has_id, RpcError::ValidationFailed, message_data(*error));
            } else if (const std::optional<std::string> failed = session_->save_provider(*form)) {
                fail(id, has_id, RpcError::StorageError, message_data(*failed));
            } else {
                reply(id, has_id, {{"ready", session_->configured()}});
                changed("provider");
            }
        }
    } else if (method == "settings.system_prompt.get") {
        if (const auto bad = check_params(params, {})) {
            fail(id, has_id, RpcError::InvalidParams, *bad);
            return;
        }
        const std::string& text = session_->system_prompt();
        reply(id, has_id,
              {{"mode", std::string{prompt_mode(*session_)}},
               {"text", text},
               {"default_text", std::string{kDefaultSystemPrompt}},
               {"max_bytes", kMaxSystemPromptBytes},
               {"warning", optional_string(system_prompt_limit_warning(
                               text, session_->config().history_limit_bytes))}});
    } else if (method == "settings.system_prompt.check" ||
               method == "settings.system_prompt.save") {
        if (const auto bad = check_params(params, {{"text"}})) {
            fail(id, has_id, RpcError::InvalidParams, *bad);
            return;
        }
        const std::string text = params->at("text").get<std::string>();
        const std::optional<std::string> error = validate_system_prompt(text);
        if (method == "settings.system_prompt.check") {
            reply(id, has_id,
                  {{"error", optional_string(error)},
                   {"warning", optional_string(system_prompt_limit_warning(
                                   text, session_->config().history_limit_bytes))},
                   {"bytes", text.size()}});
        } else if (!busy()) {
            if (error.has_value()) {
                fail(id, has_id, RpcError::ValidationFailed, message_data(*error));
            } else if (const std::optional<std::string> failed =
                           session_->save_system_prompt_text(text)) {
                fail(id, has_id, RpcError::StorageError, message_data(*failed));
            } else {
                reply(id, has_id, {{"mode", std::string{prompt_mode(*session_)}}});
                changed("system_prompt");
            }
        }
    } else if (method == "settings.search.get" || method == "settings.search.save") {
        const bool save = method == "settings.search.save";
        if (const auto bad = save ? check_params(params, {{"api_key"}}) : check_params(params, {})) {
            fail(id, has_id, RpcError::InvalidParams, *bad);
            return;
        }
        if (save && busy()) {
            return;
        }
        const Result<cli::SearchSettings> form = session_->search_form();
        if (form.is_error()) {
            fail(id, has_id, RpcError::StorageError, message_data(form.error().message));
            return;
        }
        cli::SearchSettings f = form.value();
        if (!save) {
            reply(id, has_id,
                  {{"configured", session_->has_search_key()},
                   {"locked", f.key_locked()},
                   {"key_status", f.key_status()}});
            return;
        }
        if (f.key_locked()) {
            fail(id, has_id, RpcError::ValidationFailed, message_data(kSearchKeyLocked));
            return;
        }
        (void)f.set_key(params->at("api_key").get<std::string>());
        if (const std::optional<std::string> error = f.validate()) {
            fail(id, has_id, RpcError::ValidationFailed, message_data(*error));
            return;
        }
        const auto update = f.credential_update();
        if (!update.has_value()) { // Vacía: en F2 no se guarda nada.
            fail(id, has_id, RpcError::ValidationFailed, message_data(kSearchKeyEmpty));
            return;
        }
        if (const std::optional<std::string> failed = session_->save_search_key(update->second)) {
            fail(id, has_id, RpcError::StorageError, message_data(*failed));
            return;
        }
        reply(id, has_id, {{"saved", true}});
        changed("search");
    } else if (method == "settings.gui.get") {
        if (const auto bad = check_params(params, {})) {
            fail(id, has_id, RpcError::InvalidParams, *bad);
            return;
        }
        const Result<std::optional<std::string>> value = session_->gui_settings();
        if (value.is_error()) {
            fail(id, has_id, RpcError::StorageError, message_data(value.error().message));
            return;
        }
        reply(id, has_id,
              {{"value", value.value().has_value() ? Json::parse(*value.value()) : Json(nullptr)}});
    } else if (method == "settings.gui.set") {
        if (const auto bad = check_params(params, {{"value", Param::Type::Any}})) {
            fail(id, has_id, RpcError::InvalidParams, *bad);
            return;
        }
        const Json& value = params->at("value");
        std::optional<std::string> json;
        if (!value.is_null()) {
            json = value.dump();
            if (const std::optional<std::string> problem = cli::gui_settings_problem(*json)) {
                fail(id, has_id, RpcError::ValidationFailed, message_data(*problem));
                return;
            }
        }
        if (const std::optional<std::string> failed = session_->save_gui_settings(json)) {
            fail(id, has_id, RpcError::StorageError, message_data(*failed));
            return;
        }
        reply(id, has_id, Json::object());
        changed("gui");
    } else {
        fail(id, has_id, RpcError::MethodNotFound);
    }
}

std::optional<cli::ProviderSettings> Dispatcher::provider_draft(const Json& params, const Json& id,
                                                                bool has_id) {
    const Json& draft = params.at("draft");
    std::optional<Json> bad = check_params(&draft, {{"provider"},
                                                    {"base_url"},
                                                    {"model"},
                                                    {"api_key", Param::Type::String, false}});
    std::size_t index = 0;
    if (!bad.has_value()) {
        const std::string provider = draft.at("provider").get<std::string>();
        index = cli::provider_index(provider);
        if (cli::providers()[index].id != provider) {
            // Al leer config.json un id desconocido cae en custom; aquí es un error de la GUI.
            bad = Json{{"fields", Json::array({"provider"})}};
        }
    }
    if (bad.has_value()) {
        Json fields = Json::array();
        for (const Json& field : bad->at("fields")) {
            fields.push_back("draft." + field.get<std::string>());
        }
        fail(id, has_id, RpcError::InvalidParams, {{"fields", std::move(fields)}});
        return std::nullopt;
    }
    const Result<cli::ProviderSettings> loaded = session_->provider_form();
    if (loaded.is_error()) {
        fail(id, has_id, RpcError::StorageError, message_data(loaded.error().message));
        return std::nullopt;
    }
    // Como F2: un campo fijado por CHAT_* (o la URL de un proveedor conocido)
    // no se edita, y sus setters no cambian nada.
    cli::ProviderSettings form = loaded.value();
    form.select_provider(index);
    (void)form.set_base_url(draft.at("base_url").get<std::string>());
    if (const auto key = draft.find("api_key"); key != draft.end()) {
        (void)form.set_key(key->get<std::string>());
    }
    (void)form.set_model(draft.at("model").get<std::string>());
    return form;
}

void Dispatcher::models_list(const cli::ProviderSettings& form, const Json& id, bool has_id) {
    if (!form.can_request_models()) {
        fail(id, has_id, RpcError::ValidationFailed, message_data(cli::kModelsNeedUrlAndKey));
        return;
    }
    cancel_models(); // start descarta la anterior sin llamar a su on_done.
    const std::uint64_t key = track(id, has_id);
    models_key_ = key;
    models_->start(form.request_config(session_->config()),
                   [this, key](const Result<std::vector<std::string>>& result) {
                       if (models_key_ == key) {
                           models_key_.reset();
                       }
                       finish_long(key, false, [&](const Json& request) {
                           if (result.is_ok()) {
                               reply(request, true, {{"models", result.value()}});
                           } else {
                               fail(request, true, RpcError::ValidationFailed,
                                    message_data(cli::models_error_text(result.error())));
                           }
                       });
                   });
}

void Dispatcher::cancel_models() {
    if (models_key_.has_value()) {
        const std::uint64_t key = *std::exchange(models_key_, std::nullopt);
        if (const std::optional<Json> request = untrack(key)) {
            fail(*request, true, RpcError::RequestCancelled);
        }
    }
}

void Dispatcher::addons(const std::string& method, const Json* params, const Json& id,
                        bool has_id) {
    static constexpr std::array<std::string_view, 13> kMethods{
        "addons.list",
        "addons.check_install",
        "addons.telegram.get",
        "addons.telegram.check",
        "addons.telegram.save",
        "addons.telegram.invite_code",
        "addons.telegram.invited.list",
        "addons.telegram.invited.remove",
        "service.status",
        "service.apply",
        "service.restart",
        "service.disable",
        "service.reload"};
    if (std::find(kMethods.begin(), kMethods.end(), method) == kMethods.end()) {
        fail(id, has_id, RpcError::MethodNotFound);
        return;
    }
    if (!telegram_->available()) {
        fail(id, has_id, RpcError::Unsupported,
             message_data(addon_settings::kAddonsUnavailableOnWindows));
        return;
    }
    cli::TelegramAddon& addon = *telegram_;
    const auto bad_params = [&](const std::optional<Json>& bad) {
        if (bad.has_value()) {
            fail(id, has_id, RpcError::InvalidParams, *bad);
            return true;
        }
        return false;
    };
    /// name o addon: solo los de known_addons (hoy, telegram).
    const auto known = [&](std::string_view field) {
        if (params->at(std::string{field}) == std::string{addon.info().name}) {
            return true;
        }
        fail(id, has_id, RpcError::InvalidParams, {{"fields", {std::string{field}}}});
        return false;
    };

    if (method == "addons.list") {
        if (bad_params(check_params(params, {}))) {
            return;
        }
        const std::optional<std::string> path = addon.environment().locate(addon.info());
        Json entry = install_json(addon, path);
        reply(id, has_id, {{"addons", Json::array({std::move(entry)})}});
    } else if (method == "addons.check_install") {
        if (bad_params(check_params(params, {{"name"}})) || !known("name")) {
            return;
        }
        const std::uint64_t key = track(id, has_id);
        addon.check_install([this, key] {
            finish_long(key, false, [this](const Json& request) {
                reply(request, true, install_json(*telegram_, telegram_->install()->path));
            });
        });
    } else if (method == "addons.telegram.get") {
        if (bad_params(check_params(params, {}))) {
            return;
        }
        addon.load();
        const cli::AddonSecrets& secrets = addon.secrets();
        Json labels = Json::object();
        for (std::size_t i = 0; i < cli::kAddonFieldCount; ++i) {
            const auto field = static_cast<cli::AddonField>(i);
            labels[std::string{field_name(field)}] = std::string{cli::addon_field_label(field)};
        }
        const std::string reason = read_only_reason(addon);
        reply(id, has_id,
              {{"form", form_json(addon.saved_form())},
               {"secrets",
                {{"token", cli::addon_secret_status(secrets.token, false)},
                 {"model_key", cli::addon_secret_status(secrets.model_key, false)},
                 {"search_key", cli::addon_secret_status(secrets.search_key, false)}}},
               {"labels", std::move(labels)},
               {"env_note", cli::addon_env_note(addon.environment().env_defined)},
               {"read_only", addon.read_only()},
               {"read_only_reason", non_empty_or_null(reason)},
               {"discarded", addon.discarded()},
               {"load_error", non_empty_or_null(addon.load_error())}});
    } else if (method == "addons.telegram.check" || method == "addons.telegram.save") {
        const bool save = method == "addons.telegram.save";
        if (bad_params(save ? check_params(params, {{"form", Param::Type::Object},
                                                    {"discard_confirmed", Param::Type::Boolean,
                                                     false}})
                            : check_params(params, {{"form", Param::Type::Object}}))) {
            return;
        }
        std::optional<cli::AddonForm> form;
        if (bad_params(parse_form(params->at("form"), form))) {
            return;
        }
        addon.load();
        Json field_errors = Json::object();
        for (std::size_t i = 0; i < cli::kAddonFieldCount; ++i) {
            const auto field = static_cast<cli::AddonField>(i);
            if (const std::optional<std::string> error =
                    cli::addon_field_error(*form, addon.secrets(), field)) {
                field_errors[std::string{field_name(field)}] = *error;
            }
        }
        const std::optional<std::string> form_error =
            cli::addon_form_error(*form, addon.secrets());
        const bool restart = addon.needs_restart(*form);
        if (!save) {
            reply(id, has_id,
                  {{"field_errors", std::move(field_errors)},
                   {"form_error", optional_string(form_error)},
                   {"needs_restart", restart}});
            return;
        }
        if (addon.read_only()) {
            fail(id, has_id, RpcError::ValidationFailed, message_data(read_only_reason(addon)));
            return;
        }
        if (form_error.has_value()) {
            fail(id, has_id, RpcError::ValidationFailed,
                 {{"message", *form_error}, {"field_errors", std::move(field_errors)}});
            return;
        }
        if (!addon.changed(*form)) {
            reply(id, has_id, {{"needs_restart", false}}); // Como F2: nada que escribir.
            return;
        }
        if (!addon.discarded().empty() && !params->value("discard_confirmed", false)) {
            fail(id, has_id, RpcError::ConfirmationRequired,
                 message_data(addon.discard_question()));
            return;
        }
        if (const std::optional<std::string> error = addon.save(*form)) {
            fail(id, has_id, RpcError::StorageError, message_data(*error));
            return;
        }
        reply(id, has_id, {{"needs_restart", restart}});
        write_(notification("settings.changed", {{"section", "telegram"}}));
    } else if (method == "addons.telegram.invite_code") {
        if (bad_params(check_params(params, {}))) {
            return;
        }
        const Result<std::string> code = cli::TelegramAddon::invite_code();
        if (code.is_error()) {
            fail(id, has_id, RpcError::InternalError, message_data(code.error().message));
        } else {
            // invite_code, no code: así CHAT_DEBUG_RPC lo tapa (debug_log.cpp).
            reply(id, has_id, {{"invite_code", code.value()}});
        }
    } else if (method == "addons.telegram.invited.list") {
        if (bad_params(check_params(params, {}))) {
            return;
        }
        addon.load();
        if (!addon.invited_error().empty()) {
            fail(id, has_id, RpcError::StorageError, message_data(addon.invited_error()));
        } else {
            reply(id, has_id, {{"users", user_list(addon.invited())}});
        }
    } else if (method == "addons.telegram.invited.remove") {
        if (bad_params(check_params(params, {{"user"}}))) {
            return;
        }
        const std::optional<std::int64_t> user = parse_user(params->at("user").get<std::string>());
        if (!user.has_value()) {
            fail(id, has_id, RpcError::InvalidParams, {{"fields", {"user"}}});
            return;
        }
        const std::uint64_t key = track(id, has_id);
        addon.remove_invited(*user, [this, key](const cli::TelegramAddon::Removal& removal) {
            finish_long(key, true, [this, removal](const Json& request) {
                if (removal.error.has_value()) {
                    fail(request, true, RpcError::StorageError, message_data(*removal.error));
                } else {
                    reply(request, true,
                          {{"users", user_list(telegram_->invited())},
                           {"message", removal.message}});
                }
            });
        });
    } else {
        service(method, params, id, has_id);
    }
}

void Dispatcher::service(const std::string& method, const Json* params, const Json& id,
                         bool has_id) {
    cli::TelegramAddon& addon = *telegram_;
    const bool apply = method == "service.apply";
    if (const auto bad = apply ? check_params(params, {{"addon"},
                                                       {"restart", Param::Type::Boolean, false},
                                                       {"replace_foreign", Param::Type::Boolean,
                                                        false}})
                               : check_params(params, {{"addon"}})) {
        fail(id, has_id, RpcError::InvalidParams, *bad);
        return;
    }
    if (params->at("addon") != std::string{addon.info().name}) {
        fail(id, has_id, RpcError::InvalidParams, {{"fields", {"addon"}}});
        return;
    }
    const std::string& chatbot = addon.environment().chatbot_path;
    std::error_code error;
    if (!std::filesystem::path{chatbot}.is_absolute() ||
        !std::filesystem::is_regular_file(chatbot, error)) {
        fail(id, has_id, RpcError::StorageError,
             message_data("El servicio arranca chatbot, y no está junto a chatbot-rpc: " +
                          chatbot + "."));
        return;
    }
    if (!addon.has_service()) {
        fail(id, has_id, RpcError::Unsupported, message_data(kNoHomeForService));
        return;
    }
    addon.load(); // Activado guardado (apply, restart) y mismatch (status).
    if (method == "service.restart" && !addon.saved_form().enabled) {
        fail(id, has_id, RpcError::ValidationFailed, message_data(cli::kRestartNeedsEnabled));
        return;
    }
    const std::uint64_t key = track(id, has_id);
    // La respuesta de una acción: Done/Failed → {message, ok}; NoSystemd → -32006.
    const auto action_reply = [this](const Json& request, const cli::ServiceAction& action) {
        using Kind = cli::ServiceAction::Kind;
        if (action.kind == Kind::NoSystemd) {
            fail(request, true, RpcError::Unsupported, message_data(action.message));
        } else if (action.kind == Kind::Confirm) {
            fail(request, true, RpcError::ConfirmationRequired, message_data(action.message));
        } else {
            reply(request, true, {{"message", action.message}, {"ok", action.kind == Kind::Done}});
        }
    };
    if (method == "service.status") {
        addon.refresh([this, key] {
            finish_long(key, false, [this](const Json& request) {
                reply(request, true, status_json(*telegram_));
            });
        });
    } else if (apply) {
        addon.apply(params->value("restart", false), params->value("replace_foreign", false),
                    [this, key, action_reply](const cli::TelegramAddon::Applied& applied) {
                        finish_long(key, true, [&](const Json& request) {
                            if (!applied.skipped.empty()) {
                                reply(request, true,
                                      {{"message", applied.skipped}, {"ok", false}});
                            } else {
                                action_reply(request, applied.action);
                            }
                        });
                    });
    } else {
        const auto done = [this, key, action_reply](const cli::ServiceAction& action) {
            finish_long(key, true, [&](const Json& request) { action_reply(request, action); });
        };
        if (method == "service.restart") {
            addon.restart(done);
        } else if (method == "service.disable") {
            addon.disable(done);
        } else {
            addon.reload(done);
        }
    }
}

std::uint64_t Dispatcher::track(const Json& id, bool has_id) {
    const std::uint64_t key = ++next_key_;
    if (has_id) {
        pending_.push_back({key, id});
    }
    return key;
}

std::optional<Json> Dispatcher::untrack(std::uint64_t key) {
    const auto found = std::find_if(pending_.begin(), pending_.end(),
                                    [key](const Pending& pending) { return pending.key == key; });
    if (found == pending_.end()) {
        return std::nullopt;
    }
    Json id = std::move(found->id);
    pending_.erase(found);
    return id;
}

void Dispatcher::finish_long(std::uint64_t key, bool service_changed,
                             const std::function<void(const Json&)>& respond) {
    sync(true); // El delta pendiente sale antes que la respuesta.
    if (const std::optional<Json> request = untrack(key)) {
        respond(*request);
    }
    if (service_changed) {
        write_(notification("service.changed", {{"addon", std::string{telegram_->info().name}}}));
    }
}

void Dispatcher::fail_send(cli::SendResult result, const Json& id, bool has_id,
                           std::string_view empty) {
    switch (result) {
    case cli::SendResult::Sent:
        break;
    case cli::SendResult::NotConfigured:
        fail(id, has_id, RpcError::NotConfigured, message_data(cli::kSetupNotice));
        break;
    case cli::SendResult::Busy:
        fail(id, has_id, RpcError::Busy, message_data(cli::kWaitOrCancel));
        break;
    case cli::SendResult::Empty:
        fail(id, has_id, RpcError::ValidationFailed, message_data(empty));
        break;
    case cli::SendResult::MissingSearchKey:
        fail(id, has_id, RpcError::ValidationFailed, message_data(cli::kMissingSearchKey));
        break;
    }
}

void Dispatcher::reply_file(const cli::FileOutcome& file, const Json& id, bool has_id) {
    if (file.status == cli::FileOutcome::Status::Invalid) {
        fail(id, has_id, RpcError::ValidationFailed, message_data(file.result.message));
    } else if (file.result.error) { // Sin carpeta de descargas o falló la escritura.
        fail(id, has_id, RpcError::StorageError, message_data(file.result.message));
    } else {
        reply(id, has_id, {{"path", optional_string(file.result.path)},
                           {"message", file.result.message}});
    }
}

Json Dispatcher::conversation_list() const {
    const cli::CalendarDay today =
        cli::local_day(session_->now()).value_or(cli::CalendarDay{});
    const std::string& open_id = session_->conversation().id();
    Json items = Json::array();
    for (const cli::ConversationSummary& summary : session_->store().list()) {
        items.push_back(
            {{"id", summary.id},
             {"title", summary.title},
             {"updated_at", summary.updated_at},
             {"message_count", summary.message_count},
             {"last_model", summary.last_model},
             {"readable", summary.readable},
             {"error", summary.error},
             {"group", cli::group_for(summary, today)},
             {"open_as", !open_id.empty() && summary.id == open_id ? Json(handle_) : Json(nullptr)}});
    }
    return items;
}

Json Dispatcher::command_list() const {
    const std::vector<cli::CommandAvailability> availability =
        cli::command_availability(session_->command_context());
    Json commands = Json::array();
    for (std::size_t i = 0; i < cli::kCommands.size(); ++i) {
        const cli::CommandInfo& command = cli::kCommands[i];
        commands.push_back({{"name", std::string{command.name}},
                            {"arguments", std::string{command.arguments}},
                            {"description", std::string{command.description}},
                            {"needs_argument", command.needs_argument},
                            {"available", availability[i].available},
                            {"note", std::string{availability[i].note}}});
    }
    return commands;
}

void Dispatcher::run_command(const std::string& text, const Json& id, bool has_id) {
    using Type = cli::SubmitOutcome::Type;
    const cli::SubmitOutcome outcome = session_->submit(text);
    if (outcome.sent.has_value()) { // Texto para el modelo: como chat.send.
        const bool search = outcome.command != cli::ParsedCommand::Type::Normal;
        if (*outcome.sent == cli::SendResult::Sent) {
            reply(id, has_id, {{"kind", search ? "search" : "message"}, {"clear", true}});
        } else {
            // /buscar sin consulta: el uso de la TUI.
            fail_send(*outcome.sent, id, has_id, search ? cli::kSearchUsage : kEmptyMessage);
        }
        return;
    }
    switch (outcome.type) {
    case Type::Copy: {
        // La GUI siempre puede copiar: el aviso va al historial, como en la TUI.
        const std::string message = cli::copied_message(outcome.block);
        session_->report({message, false});
        reply(id, has_id,
              {{"kind", "copy"},
               {"number", outcome.block.number},
               {"language", outcome.block.info},
               {"text", outcome.block.code},
               {"message", message},
               {"clear", outcome.clear}});
        break;
    }
    case Type::Done:
        reply(id, has_id,
              {{"kind", outcome.command == cli::ParsedCommand::Type::Save ? "save" : "export"},
               {"path", optional_string(outcome.action.path)},
               {"message", outcome.action.message},
               {"error", outcome.action.error},
               {"clear", outcome.clear}});
        break;
    case Type::Status:
    case Type::Started: // Sin sent no pasan.
    case Type::Ignored:
        reply(id, has_id, {{"kind", "status"}, {"message", outcome.status}, {"clear", false}});
        break;
    }
}

void Dispatcher::reply(const Json& id, bool has_id, const Json& result) {
    if (has_id) {
        write_(response(id, result));
    }
}

void Dispatcher::fail(const Json& id, bool has_id, RpcError code, const Json& data) {
    if (has_id) {
        write_(error_response(id, code, data));
    }
}

void Dispatcher::initialize(const Json* params, const Json& id, bool has_id) {
    if (state_ == State::Running) {
        fail(id, has_id, RpcError::InvalidRequest, message_data(kAlreadyInitialized));
        return;
    }
    if (const auto bad = check_params(params, {{"protocol_version", Param::Type::Integer},
                                               {"client", Param::Type::Object, false}})) {
        fail(id, has_id, RpcError::InvalidParams, *bad);
        return;
    }
    if (params->at("protocol_version") != kProtocolVersion) {
        fail(id, has_id, RpcError::ProtocolMismatch, {{"server", kProtocolVersion}});
        return;
    }
    if (info_.startup_error.has_value()) {
        fail(id, has_id, RpcError::StartupFailed, message_data(*info_.startup_error));
        return;
    }
    alive_ = std::make_shared<int>(0);
    // Lo que el runner encole mientras se destruye la sesión (join) se queda
    // en la cola de main: al sacarlo, la sesión ya no existe y no corre.
    cli::Session::Post guarded = [post = post_, alive = std::weak_ptr<int>{alive_}](
                                     cli::RequestRunner::Task task) {
        post([alive, task = std::move(task)] {
            if (!alive.expired()) {
                task();
            }
        });
    };
    models_ = std::make_unique<cli::ModelsLoader>(models_transport_, guarded);
    telegram_ = make_addons_(guarded);
    session_ = make_session_(std::move(guarded), [this](cli::Session::Finished finished) {
        finished_ = std::move(finished);
    });
    state_ = State::Running;
    start_conversation();
    const bool ready = session_->configured();
    Json capabilities{{"search", true}, {"addons", Json::array()}, {"service", nullptr}};
    if (telegram_->available()) {
        for (const addon_settings::AddonInfo& addon : addon_settings::known_addons()) {
            capabilities["addons"].push_back(std::string{addon.name});
        }
        capabilities["service"] = "systemd";
    }
    reply(id, has_id,
          {{"protocol_version", kProtocolVersion},
           {"server", {{"name", std::string{kServerName}}, {"version", info_.version}}},
           {"platform", info_.platform},
           {"portable", info_.portable},
           {"conversation", handle_},
           {"paths",
            {{"config", optional_string(info_.config_path)},
             {"conversations", optional_string(info_.conversations_path)},
             {"downloads", optional_string(info_.downloads_path)}}},
           {"ready", ready},
           {"problem", ready ? Json(nullptr) : Json(std::string{cli::kSetupNotice})},
           {"warnings", session_->startup_warnings()},
           // Lo que soporta este servidor; no cambia durante la sesión.
           {"capabilities", capabilities}});
}

void Dispatcher::shutdown(const Json& id, bool has_id) {
    if (session_ != nullptr) {
        // Una respuesta en curso se cancela sin chat.finished: el cliente se va.
        write_(notification("conversation.closed",
                            {{"conversation", handle_}, {"reason", "shutdown"}}));
    }
    close_session();
    state_ = State::ShutDown;
    reply(id, has_id, Json::object());
}

void Dispatcher::close_session() {
    alive_.reset();
    pending_.clear(); // Pendientes al apagar o en EOF: sin respuesta, como chat.send.
    models_key_.reset();
    models_.reset();   // Cancela y hace join, antes que la sesión.
    telegram_.reset(); // Cancela el programa en curso y hace join.
    session_.reset();  // Cancela y hace join.
    finished_.reset();
    pending_since_.reset();
}

bool Dispatcher::check_handle(const Json& params, const Json& id, bool has_id) {
    if (params.at("conversation") != handle_) {
        fail(id, has_id, RpcError::UnknownConversation);
        return false;
    }
    return true;
}

void Dispatcher::start_conversation() {
    handle_ = "c" + std::to_string(++handle_number_);
    rev_ = 0;
    seen_ = remember(session_->conversation().entries());
    phase_ = current_phase();
    pending_since_.reset();
}

Phase Dispatcher::current_phase() const {
    if (!session_->busy()) {
        return Phase::Idle;
    }
    if (session_->searching()) {
        return Phase::Searching;
    }
    const std::vector<cli::Entry>& entries = session_->conversation().entries();
    if (!entries.empty() && entries.back().kind == cli::EntryKind::Assistant &&
        entries.back().in_progress) {
        return Phase::Streaming;
    }
    return Phase::Thinking;
}

Json Dispatcher::snapshot() const {
    const cli::Conversation& conversation = session_->conversation();
    Json entries = Json::array();
    for (const cli::Entry& entry : conversation.entries()) {
        entries.push_back(encode_entry(entry));
    }
    return {{"conversation", handle_},
            {"rev", rev_},
            {"id", non_empty_or_null(conversation.id())},
            {"title", conversation.title()},
            {"created_at", non_empty_or_null(conversation.created_at())},
            {"last_model", conversation.last_model()},
            {"phase", std::string{phase_name(phase_)}},
            {"entries", std::move(entries)}};
}

Json Dispatcher::status() const {
    const bool ready = session_->configured();
    return {{"ready", ready},
            {"problem", ready ? Json(nullptr) : Json(std::string{cli::kSetupNotice})},
            {"busy", session_->busy()},
            {"conversation", handle_},
            {"env_locks", info_.env_locks},
            {"has_search_key", session_->has_search_key()}};
}

void Dispatcher::sync(bool flush) {
    if (session_ == nullptr) {
        return;
    }
    const cli::Conversation& conversation = session_->conversation();
    const std::vector<cli::Entry>& entries = conversation.entries();
    const EntriesDiff diff = diff_entries(seen_, entries);
    const Phase phase = current_phase();
    const bool other =
        diff.updated || diff.added(seen_) || phase != phase_ || finished_.has_value();
    if (!diff.grown && !other) {
        return;
    }
    if (!other && !flush) {
        // Solo texto agregado a la respuesta en curso: espera a juntarse.
        if (!pending_since_.has_value()) {
            pending_since_ = now_();
        }
        return;
    }
    // El texto pendiente sale antes que cualquier otro evento, también
    // antes de entry_updated (que de todos modos trae la entrada completa).
    if (diff.grown) {
        const std::size_t last = seen_.count - 1;
        emit_conversation(
            "conversation.entry_delta",
            {{"index", last}, {"text", entries[last].text.substr(seen_.last_text_size)}});
    }
    if (diff.updated) {
        const std::size_t last = seen_.count - 1;
        emit_conversation("conversation.entry_updated",
                          {{"index", last}, {"entry", encode_entry(entries[last])}});
    }
    for (std::size_t index = seen_.count; index < diff.added_end; ++index) {
        emit_conversation("conversation.entry_added",
                          {{"index", index}, {"entry", encode_entry(entries[index])}});
    }
    seen_ = remember(entries);
    pending_since_.reset();
    if (finished_.has_value() && finished_->saved) {
        emit_conversation("conversation.saved",
                          {{"id", conversation.id()}, {"title", conversation.title()}});
        write_(notification("conversations.changed", Json::object()));
    }
    if (phase != phase_) {
        phase_ = phase;
        emit_conversation("conversation.phase", {{"phase", std::string{phase_name(phase)}}});
    }
    if (finished_.has_value()) {
        const cli::Session::Finished finished = std::move(*finished_);
        finished_.reset();
        const std::string_view outcome = outcome_name(finished);
        emit_conversation(
            "chat.finished",
            {{"outcome", std::string{outcome}},
             {"finish_reason", finished.finish_reason},
             {"dropped_messages", finished.dropped},
             {"error", outcome == "error" ? encode_error(*finished.error) : Json(nullptr)},
             {"restore_text", optional_string(finished.restored)}});
    }
}

void Dispatcher::emit_conversation(std::string_view method, Json params) {
    Json full{{"conversation", handle_}, {"rev", ++rev_}};
    full.update(params);
    write_(notification(method, full));
}

} // namespace chatbot::rpc
