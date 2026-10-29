#include "dispatcher.h"

#include "block_actions.h"
#include "code_blocks.h"
#include "commands.h"
#include "search_settings.h"
#include "sidebar.h"

#include <cstdint>
#include <exception>
#include <limits>
#include <utility>

namespace chatbot::rpc {

namespace {

constexpr std::string_view kServerName = "chatbot-rpc";
constexpr std::string_view kEmptyMessage = "El mensaje está vacío.";
constexpr std::string_view kAlreadyInitialized = "initialize ya se llamó.";
constexpr std::string_view kAlreadyShutDown = "El servidor ya se apagó: solo queda exit.";

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

} // namespace

Dispatcher::Dispatcher(ServerInfo info, SessionFactory make_session, cli::Session::Post post,
                       Write write, Now now)
    : info_(std::move(info)), make_session_(std::move(make_session)), post_(std::move(post)),
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
        // Sin peticiones largas en esta versión: no hay nada que cancelar.
        if (const auto bad = check_params(params, {{"id", Param::Type::IntegerOrString}})) {
            fail(id, has_id, RpcError::InvalidParams, *bad);
        } else {
            reply(id, has_id, Json::object());
        }
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
    } else {
        fail(id, has_id, RpcError::MethodNotFound);
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
    session_ = make_session_(std::move(guarded), [this](cli::Session::Finished finished) {
        finished_ = std::move(finished);
    });
    state_ = State::Running;
    start_conversation();
    const bool ready = session_->configured();
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
           {"capabilities",
            {{"search", true}, {"addons", Json::array()}, {"service", nullptr}}}});
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
    session_.reset(); // Cancela y hace join.
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
