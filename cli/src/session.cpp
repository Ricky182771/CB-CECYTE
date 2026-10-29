#include "session.h"

#include "command_parser.h"
#include "downloads.h"

#include "chatbot/credentials.h"
#include "chatbot/error.h"
#include "chatbot/platform.h"
#include "chatbot/search_context.h"
#include "chatbot/system_prompt.h"

#include <algorithm>
#include <cctype>
#include <utility>
#include <vector>

namespace chatbot::cli {

namespace {

/// Una variable vacía cuenta como no definida (igual que en load_config).
std::optional<std::string> non_empty(const chatbot::EnvLookup& env, std::string_view name) {
    const std::string_view value = env(name);
    return value.empty() ? std::nullopt : std::optional<std::string>{value};
}

void append_warning(std::vector<std::string>& warnings, std::string warning) {
    if (!warning.empty()) {
        warnings.push_back(std::move(warning));
    }
}

SubmitOutcome outcome(SubmitOutcome::Type type, std::string status = {}, bool clear = false) {
    SubmitOutcome result;
    result.type = type;
    result.status = std::move(status);
    result.clear = clear;
    return result;
}

ChatError config_error(std::string message) {
    return ChatError{ErrorKind::Config, 0, std::move(message), std::nullopt};
}

} // namespace

Session::Session(SessionEnvironment environment, Config initial, Post post,
                 OnFinished on_finished)
    : env_(std::move(environment)), config_(std::move(initial)), post_(std::move(post)),
      on_finished_(std::move(on_finished)), system_prompt_(kDefaultSystemPrompt),
      store_(env_.data_dir), conversation_(system_prompt_) {
    // Instrucciones de config.json ("system_prompt"). Una llave que no es
    // cadena usa las predeterminadas y deja un aviso.
    if (env_.config_path.has_value()) {
        ResolvedSystemPrompt resolved =
            resolve_system_prompt(load_system_prompt_value(*env_.config_path));
        system_prompt_ = std::move(resolved.text);
        append_warning(startup_warnings_, std::move(resolved.warning));
        conversation_ = Conversation{system_prompt_};
    }
    std::string search_warning;
    search_provider_ = load_search(search_warning);
    append_warning(startup_warnings_, std::move(search_warning));
    for (const std::string& warning : startup_warnings_) {
        startup_warning_ += (startup_warning_.empty() ? "" : " ") + warning;
    }
    if (!validate_config(config_).has_value()) {
        connect();
    }
}

void Session::connect() {
    runner_.reset();
    client_ = env_.make_client(config_);
    runner_ = std::make_unique<RequestRunner>(*client_, config_.history_limit_bytes, post_);
    model_ = config_.model;
}

std::shared_ptr<SearchProvider> Session::load_search(std::string& warning) const {
    if (!env_.credentials_path.has_value() &&
        !non_empty(env_.env, "CHAT_SEARCH_API_KEY").has_value()) {
        return nullptr;
    }
    const Result<std::string> key =
        load_search_api_key(env_.env, env_.credentials_path.value_or(""));
    if (key.is_error()) {
        warning = key.error().message;
        return nullptr;
    }
    if (key.value().empty()) {
        return nullptr;
    }
    return env_.make_search(key.value());
}

CommandContext Session::command_context() {
    CommandContext context;
    context.configured = configured();
    context.has_search_key = search_provider_ != nullptr;
    context.has_code_blocks = !code_blocks().blocks().empty();
    context.has_turns = conversation_.has_turns();
    context.busy = busy();
    return context;
}

const CodeBlockIndex& Session::code_blocks() {
    code_blocks_.update(conversation_.entries());
    return code_blocks_;
}

SubmitOutcome Session::submit(std::string_view text) {
    using Type = ParsedCommand::Type;
    const ParsedCommand command = parse_command(text);
    SubmitOutcome result;
    switch (command.type) {
    case Type::Copy:
    case Type::CopyUsage:
    case Type::Save:
    case Type::SaveUsage:
    case Type::Export:
    case Type::ExportUsage:
        // No necesitan modelo: funcionan aunque falte la configuración.
        result = run_local_command(command);
        result.command = command.type;
        return result;
    default:
        break;
    }
    // /buscar sin consulta es una búsqueda vacía: send da Empty en el mismo
    // orden que antes (después de la configuración y la respuesta en curso).
    const bool search = command.type == Type::Search || command.type == Type::SearchEmpty;
    const SendResult sent =
        send(text, search ? std::optional<std::string>{command.text} : std::nullopt);
    switch (sent) {
    case SendResult::Sent:
        result = outcome(SubmitOutcome::Type::Started, {}, true);
        break;
    case SendResult::NotConfigured:
        result = outcome(SubmitOutcome::Type::Status, std::string{kSetupNotice});
        break;
    case SendResult::Busy:
        break; // Enter no hace nada mientras hay una respuesta en curso.
    case SendResult::Empty:
        // /buscar sin consulta: el uso, y el texto se queda en la caja.
        if (search) {
            result = outcome(SubmitOutcome::Type::Status, std::string{kSearchUsage});
        }
        break; // Caja vacía: Ignored.
    case SendResult::MissingSearchKey:
        result = outcome(SubmitOutcome::Type::Status, std::string{kMissingSearchKey});
        break;
    }
    result.command = command.type;
    result.sent = sent;
    return result;
}

SendResult Session::send(std::string_view text, std::optional<std::string> search) {
    if (runner_ == nullptr) {
        return SendResult::NotConfigured;
    }
    if (runner_->busy()) {
        return SendResult::Busy;
    }
    if (search.has_value() && std::all_of(search->begin(), search->end(), [](char c) {
            return std::isspace(static_cast<unsigned char>(c)) != 0;
        })) {
        return SendResult::Empty;
    }
    if (search.has_value() && search_provider_ == nullptr) {
        return SendResult::MissingSearchKey;
    }
    std::optional<std::vector<Message>> messages = conversation_.submit(text);
    if (!messages.has_value()) {
        return SendResult::Empty;
    }
    const auto on_delta = [this](std::string delta) { conversation_.append_delta(delta); };
    const auto on_done = [this](Result<CompletionInfo> result, std::size_t dropped) {
        searching_ = false;
        last_dropped_ = dropped;
        Finished finished;
        finished.dropped = dropped;
        if (result.is_ok()) {
            finished.restored = conversation_.finish_success(result.value().finish_reason, model_);
            if (finished.restored.has_value()) {
                finished.error =
                    ChatError{ErrorKind::BadResponse, 0, std::string{kEmptyAnswer}, std::nullopt};
            } else {
                finished.finish_reason = result.value().finish_reason;
            }
        } else {
            finished.error = result.error();
            finished.restored = conversation_.finish_error(result.error());
        }
        if (!finished.restored.has_value()) {
            // Par usuario/asistente completo: es lo único que se guarda.
            finished.saved = save_conversation(conversation_, store_);
        }
        on_finished_(std::move(finished));
    };
    if (!search.has_value()) {
        runner_->start(std::move(*messages), on_delta, on_done);
    } else {
        // La fecha se guarda con la búsqueda (AAAA-MM-DD) y el bloque la
        // lleva en español.
        const std::string date = local_iso_date(env_.now());
        searching_ = true;
        runner_->start_with_search(
            std::move(*messages), *search, search_provider_, spanish_date(date),
            [this, date](RequestRunner::SearchContext found) {
                searching_ = false;
                (void)conversation_.attach_search(std::move(found.response), date,
                                                  std::move(found.block));
            },
            on_delta, on_done);
    }
    return SendResult::Sent;
}

SubmitOutcome Session::run_local_command(const ParsedCommand& command) {
    using Type = ParsedCommand::Type;
    const auto status = [](std::string text) {
        return outcome(SubmitOutcome::Type::Status, std::move(text));
    };
    // Lo que no se pudo ni intentar va a la línea de estado; el resultado,
    // al historial, y la caja se vacía si salió bien.
    const auto done = [this, &status](FileOutcome file) {
        if (file.status != FileOutcome::Status::Attempted) {
            return status(std::move(file.result.message));
        }
        report(file.result);
        SubmitOutcome result = outcome(SubmitOutcome::Type::Done, {}, !file.result.error);
        result.action = std::move(file.result);
        return result;
    };
    if (busy()) {
        return status("Espera a que termine la respuesta");
    }
    switch (command.type) {
    case Type::CopyUsage:
        return status(std::string{kCopyUsage});
    case Type::SaveUsage:
        return status(std::string{kSaveUsage});
    case Type::ExportUsage:
        return status(std::string{kExportUsage});
    case Type::Export:
        return done(export_to_downloads());
    case Type::Save:
        return done(save_code_block(command.block, command.file_name));
    default:
        break;
    }
    const CodeBlockChoice choice = choose_code_block(code_blocks().blocks(), command.block);
    if (choice.block == nullptr) {
        return status(choice.problem);
    }
    // Copy: la caja se vacía aunque no se haya podido copiar.
    SubmitOutcome copy = outcome(SubmitOutcome::Type::Copy, {}, true);
    copy.block = *choice.block;
    return copy;
}

FileOutcome Session::save_code_block(std::optional<int> number, std::string_view file_name) {
    const CodeBlockChoice choice = choose_code_block(code_blocks().blocks(), number);
    if (choice.block == nullptr) {
        return {FileOutcome::Status::Invalid, {choice.problem, true}};
    }
    if (!file_name.empty()) {
        if (std::optional<std::string> reason = validate_file_name(file_name)) {
            return {FileOutcome::Status::Invalid, {std::move(*reason), true}};
        }
    }
    const std::optional<std::string> base = env_.download_base();
    if (!base.has_value()) {
        return {FileOutcome::Status::NoFolder,
                {std::string{no_download_dir_message(current_os())}, true}};
    }
    return {FileOutcome::Status::Attempted,
            save_block(*choice.block, file_name, *base, env_.home)};
}

FileOutcome Session::export_to_downloads() {
    if (!conversation_.has_turns()) {
        return {FileOutcome::Status::Invalid, {"Todavía no hay respuestas que exportar.", true}};
    }
    const std::optional<std::string> base = env_.download_base();
    if (!base.has_value()) {
        return {FileOutcome::Status::NoFolder,
                {std::string{no_download_dir_message(current_os())}, true}};
    }
    return {FileOutcome::Status::Attempted,
            export_conversation(conversation_, env_.now(), *base, env_.home)};
}

void Session::cancel() {
    if (runner_ != nullptr) {
        runner_->cancel(); // Sin petición en curso no hace nada.
    }
}

void Session::report(const ActionResult& result) {
    if (result.error) {
        conversation_.add_error(result.message);
    } else {
        conversation_.add_notice(result.message);
    }
}

std::optional<std::string> Session::new_conversation() {
    if (busy()) {
        return std::string{kWaitOrCancel};
    }
    // La actual ya quedó guardada si tenía pares.
    conversation_ = Conversation{system_prompt_};
    last_dropped_ = 0;
    return std::nullopt;
}

std::optional<std::string> Session::open(const std::string& id) {
    if (busy()) {
        return std::string{kWaitOrCancel};
    }
    const LoadResult loaded = store_.load(id);
    if (!loaded.conversation.has_value()) {
        return loaded.error;
    }
    conversation_ = Conversation::from_stored(*loaded.conversation, system_prompt_);
    const std::string used = conversation_.last_model();
    if (!used.empty() && !model_.empty() && used != model_) {
        conversation_.add_notice("Esta conversación usó " + used + "; se continúa con " + model_ +
                                 ".");
    }
    last_dropped_ = 0;
    return std::nullopt;
}

std::optional<std::string> Session::remove(const std::string& id) {
    if (busy()) {
        return std::string{kWaitOrCancel};
    }
    if (const std::optional<std::string> error = store_.remove(id)) {
        return "No se pudo borrar la conversación: " + *error;
    }
    if (id == conversation_.id()) {
        (void)new_conversation(); // Se borró la que estaba abierta.
    }
    return std::nullopt;
}

Result<ProviderSettings> Session::provider_form() const {
    if (!env_.config_path.has_value() || !env_.credentials_path.has_value()) {
        return config_error(missing_config_dir_message(current_os()));
    }
    const Result<ConfigFileValues> values = load_config_file_values(*env_.config_path);
    if (values.is_error()) {
        return values.error();
    }
    const Result<Credentials> credentials = load_credentials(*env_.credentials_path);
    if (credentials.is_error()) {
        return credentials.error();
    }
    SettingsEnv env;
    env.base_url = non_empty(env_.env, "CHAT_BASE_URL");
    env.model = non_empty(env_.env, "CHAT_MODEL");
    env.api_key = non_empty(env_.env, "CHAT_API_KEY");
    return ProviderSettings(values.value(), credentials.value(), std::move(env));
}

Result<SearchSettings> Session::search_form() const {
    if (!env_.credentials_path.has_value()) {
        return config_error(missing_config_dir_message(current_os()));
    }
    const Result<Credentials> credentials = load_credentials(*env_.credentials_path);
    if (credentials.is_error()) {
        return credentials.error();
    }
    const auto saved = credentials.value().keys.find(std::string{kSearchCredentialsKey});
    return SearchSettings(saved != credentials.value().keys.end() ? saved->second : std::string{},
                          non_empty(env_.env, "CHAT_SEARCH_API_KEY"));
}

std::optional<std::string> Session::save_provider(const ProviderSettings& form) {
    if (!env_.config_path.has_value() || !env_.credentials_path.has_value()) {
        return missing_config_dir_message(current_os());
    }
    // Primero la key: si falla, config.json no apunta a un proveedor sin key.
    if (const auto update = form.credential_update()) {
        const Result<Credentials> saved = load_credentials(*env_.credentials_path);
        if (saved.is_error()) {
            return saved.error().message;
        }
        Credentials credentials = saved.value();
        credentials.keys[update->first] = update->second;
        if (const auto error = save_credentials(*env_.credentials_path, credentials)) {
            return error->message;
        }
    }
    if (const auto error = save_config_file(*env_.config_path, form.file_values())) {
        return error->message;
    }
    // Lo que quedó guardado más el entorno, validado como al arrancar.
    ConfigOptions options;
    options.env = env_.env;
    options.path = [path = env_.config_path] { return path; };
    options.credentials_path = [path = env_.credentials_path] { return path; };
    const Result<Config> fresh = load_config(options);
    if (fresh.is_error()) {
        return fresh.error().message;
    }
    config_ = fresh.value();
    connect();
    conversation_.add_notice("Ahora se usa " + model_ + " de " + form.provider_label() + ".");
    return std::nullopt;
}

std::optional<std::string> Session::save_system_prompt(const std::optional<std::string>& stored) {
    if (!env_.config_path.has_value()) {
        return missing_config_dir_message(current_os());
    }
    if (const auto error = chatbot::save_system_prompt(*env_.config_path, stored)) {
        return error->message;
    }
    // Valen desde el siguiente mensaje, sin perder la conversación (la
    // configuración no se abre con una respuesta en curso).
    system_prompt_ = stored.value_or(std::string{kDefaultSystemPrompt});
    (void)conversation_.set_system_prompt(system_prompt_);
    conversation_.add_notice(system_prompt_.empty()
                                 ? "Sin instrucciones del sistema desde el siguiente mensaje."
                                 : "Instrucciones del sistema actualizadas: valen desde el "
                                   "siguiente mensaje.");
    if (const std::optional<std::string> warning =
            system_prompt_limit_warning(system_prompt_, config_.history_limit_bytes)) {
        conversation_.add_notice(*warning);
    }
    return std::nullopt;
}

std::optional<std::string> Session::save_search_key(const std::string& key) {
    if (!env_.credentials_path.has_value()) {
        return missing_config_dir_message(current_os());
    }
    const Result<Credentials> saved = load_credentials(*env_.credentials_path);
    if (saved.is_error()) {
        return saved.error().message;
    }
    Credentials credentials = saved.value();
    credentials.keys[std::string{kSearchCredentialsKey}] = key;
    if (const auto error = save_credentials(*env_.credentials_path, credentials)) {
        return error->message;
    }
    std::string warning;
    search_provider_ = load_search(warning);
    if (!warning.empty()) {
        return warning;
    }
    conversation_.add_notice("Key de búsqueda guardada: usa /buscar <consulta>.");
    return std::nullopt;
}

} // namespace chatbot::cli
