#include "chatbot/bot/bot.h"

#include "chatbot/bot/commands.h"
#include "chatbot/bot/replies.h"
#include "chatbot/bot/split.h"

#include "chatbot/error.h"
#include "chatbot/markdown/markdown.h"
#include "chatbot/search_context.h"
#include "chatbot/trimmed_completion.h"

#include <algorithm>
#include <string_view>
#include <system_error>
#include <utility>

namespace chatbot::bot {
namespace {

/// No autorizados que se anotan en el registro (una vez por usuario).
/// Después ya no se anotan: con el bot expuesto, el registro no debe crecer
/// sin fin.
constexpr std::size_t kMaxReportedDenied = 1000;
/// Intentos con un código incorrecto que se anotan por usuario.
constexpr unsigned kMaxReportedRejections = 3;
/// Tope del detalle de un error en el registro.
constexpr std::size_t kMaxLogDetail = 200;

/// Mismo criterio que split_markdown: una respuesta que no deja ningún
/// pedazo es una respuesta vacía.
bool is_blank(std::string_view text) {
    return text.find_first_not_of(" \t\r\n\f\v") == std::string_view::npos;
}

/// Texto que va en una línea del registro o de "Fuentes": sin controles
/// (md::sanitize), sin saltos de línea ni comillas y con un tope de bytes
/// que no parte caracteres.
std::string one_line(std::string_view text, std::size_t max_bytes) {
    std::string clean = md::sanitize(text);
    std::replace(clean.begin(), clean.end(), '\n', ' ');
    std::replace(clean.begin(), clean.end(), '"', '\'');
    constexpr std::string_view kEllipsis = "...";
    if (clean.size() > max_bytes) {
        // El tope incluye los puntos suspensivos.
        std::size_t end = max_bytes > kEllipsis.size() ? max_bytes - kEllipsis.size() : 0;
        while (end > 0 && (static_cast<unsigned char>(clean[end]) & 0xC0) == 0x80) {
            --end;
        }
        clean.resize(end);
        clean += kEllipsis;
    }
    return clean;
}

long long elapsed_ms(std::chrono::steady_clock::time_point since) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                 since)
        .count();
}

const char* command_name(BotCommand::Type type) {
    switch (type) {
    case BotCommand::Type::Normal:
        return "texto";
    case BotCommand::Type::Start:
        return "start";
    case BotCommand::Type::Help:
        return "ayuda";
    case BotCommand::Type::New:
        return "nuevo";
    case BotCommand::Type::Search:
    case BotCommand::Type::SearchEmpty:
        return "buscar";
    }
    return "?";
}

/// "Fuentes:" con un renglón por resultado: "[n] título — fecha" y la URL
/// abajo. Los datos vienen de la búsqueda, no del texto del modelo.
std::string sources_text(const std::vector<SearchResult>& results) {
    std::string text{kSourcesTitle};
    for (std::size_t i = 0; i < results.size(); ++i) {
        const SearchResult& result = results[i];
        text += "\n[" + std::to_string(i + 1) + "] ";
        const std::string title = one_line(result.title, 300);
        text += title.empty() ? std::string{"(sin título)"} : title;
        if (!result.published_date.empty()) {
            text += " — " + one_line(result.published_date, 40);
        }
        text += "\n" + one_line(result.url, 2000);
    }
    return text;
}

} // namespace

Bot::Bot(ValidBotConfig config, BotDependencies dependencies)
    : config_(config.get()), dependencies_(std::move(dependencies)),
      limit_(dependencies_.outbox.message_limit()), access_(config_),
      sessions_(config_.system_prompt, config_.session_idle_timeout),
      counter_(config_.daily_message_limit) {
    workers_.reserve(config_.max_concurrent_responses);
    try {
        for (std::size_t i = 0; i < config_.max_concurrent_responses; ++i) {
            workers_.emplace_back([this] { work(); });
        }
    } catch (const std::system_error&) {
        log(LogLevel::Error, "no se pudieron crear todos los hilos de trabajo creados=" +
                                 std::to_string(workers_.size()));
        if (workers_.empty()) {
            const std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true; // Sin hilos nadie respondería: el bot no atiende.
        }
    }
    log(LogLevel::Info,
        "inicio respuestas_simultaneas=" + std::to_string(workers_.size()) +
            " autorizados=" + std::to_string(config_.allowed_users.size()) +
            " codigo=" + (config_.invite_code.empty() ? "no" : "si") +
            " busqueda=" + (dependencies_.make_search ? "si" : "no"));
}

Bot::~Bot() {
    shutdown();
}

std::string Bot::today() const {
    return local_iso_date(std::chrono::system_clock::to_time_t(dependencies_.clock.now()));
}

void Bot::log(LogLevel level, const std::string& line) {
    try {
        dependencies_.log.write(level, line);
    } catch (...) {
        // El registro nunca debe tumbar al bot.
    }
}

void Bot::handle(const IncomingMessage& message) {
    const TimePoint now = dependencies_.clock.now();
    const std::string day = today();
    const BotCommand command = parse_bot_command(message.text);
    const std::string ids = "chat=" + std::to_string(message.chat) +
                            " usuario=" + std::to_string(message.user);
    const bool search_available = static_cast<bool>(dependencies_.make_search);

    // Lo que sale se manda y se anota después de soltar el candado.
    std::vector<std::string> replies;
    std::vector<std::pair<LogLevel, std::string>> lines;
    bool queued = false;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) {
            return;
        }
        if (const std::size_t expired = sessions_.expire(now); expired > 0) {
            lines.emplace_back(LogLevel::Info, "sesiones_borradas n=" + std::to_string(expired));
        }

        const auto enqueue = [&](std::string text, std::optional<std::string> query) {
            Session& session = sessions_.get(message.chat, now);
            if (session.busy) {
                replies.emplace_back(kWaitPrevious);
                return;
            }
            if (!counter_.try_count(message.user, day)) {
                replies.push_back(daily_limit_reached(counter_.limit()));
                lines.emplace_back(LogLevel::Info, "tope_diario " + ids);
                return;
            }
            session.busy = true;
            session.last_activity = now;
            queue_.push_back(Job{message.chat, message.user, message.platform, day,
                                 std::move(text), std::move(query), session.history()});
            queued = true;
            lines.emplace_back(LogLevel::Info, "en_fila " + ids +
                                                   " fila=" + std::to_string(queue_.size()) +
                                                   " en_curso=" + std::to_string(running_));
        };

        if (!access_.is_allowed(message.user)) {
            if (command.type == BotCommand::Type::Start &&
                access_.redeem(message.user, command.argument)) {
                replies.emplace_back(kWelcome);
                lines.emplace_back(LogLevel::Info, "invitacion_aceptada " + ids);
            } else {
                // Cada usuario se anota una vez, y sus códigos incorrectos
                // hasta kMaxReportedRejections: un extraño que insiste no
                // hace crecer el registro.
                auto found = denied_.find(message.user);
                if (found == denied_.end() && denied_.size() < kMaxReportedDenied) {
                    found = denied_.emplace(message.user, 0U).first;
                    lines.emplace_back(LogLevel::Warning, "no_autorizado " + ids);
                    if (denied_.size() == kMaxReportedDenied) {
                        lines.emplace_back(LogLevel::Warning,
                                           "no_autorizado demasiados usuarios: ya no se anotan");
                    }
                }
                if (found != denied_.end() && command.type == BotCommand::Type::Start &&
                    !command.argument.empty() && found->second < kMaxReportedRejections) {
                    ++found->second;
                    lines.emplace_back(LogLevel::Warning,
                                       "invitacion_rechazada " + ids +
                                           " intento=" + std::to_string(found->second));
                }
                replies.emplace_back(kAccessDenied);
            }
        } else {
            lines.emplace_back(LogLevel::Info,
                               "mensaje plataforma=" + one_line(message.platform, 32) + " " + ids +
                                   " bytes=" + std::to_string(message.text.size()) +
                                   " comando=" + command_name(command.type));
            switch (command.type) {
            case BotCommand::Type::Start:
                replies.emplace_back(kWelcome);
                break;
            case BotCommand::Type::Help:
                replies.push_back(help_text(search_available, config_.daily_message_limit,
                                            config_.session_idle_timeout));
                break;
            case BotCommand::Type::New:
                replies.emplace_back(sessions_.reset(message.chat) ? kNewConversation
                                                                   : kWaitPrevious);
                break;
            case BotCommand::Type::SearchEmpty:
                replies.emplace_back(search_available ? kSearchUsage : kSearchUnavailable);
                break;
            case BotCommand::Type::Search:
                if (!search_available) {
                    replies.emplace_back(kSearchUnavailable);
                } else {
                    enqueue({}, command.argument);
                }
                break;
            case BotCommand::Type::Normal:
                if (command.argument.empty()) {
                    replies.emplace_back(kTextOnly);
                } else {
                    enqueue(command.argument, std::nullopt);
                }
                break;
            }
        }
    }
    if (queued) {
        work_ready_.notify_one();
    }
    for (const auto& [level, line] : lines) {
        log(level, line);
    }
    for (const std::string& reply : replies) {
        (void)deliver(message.chat, reply, TextFormat::Plain);
    }
}

void Bot::work() {
    while (true) {
        std::optional<Job> job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            work_ready_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (stopping_) {
                return;
            }
            job = std::move(queue_.front());
            queue_.pop_front();
            ++running_;
        }
        run(*job);
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            --running_;
        }
        idle_.notify_all();
    }
}

void Bot::run(const Job& job) {
    const auto started = std::chrono::steady_clock::now();
    const std::string ids =
        "chat=" + std::to_string(job.chat) + " usuario=" + std::to_string(job.user);
    Outbox& outbox = dependencies_.outbox;
    try {
        outbox.set_typing(job.chat, true);
    } catch (...) {
        // "Escribiendo…" es opcional.
    }

    std::optional<ChatError> failure;
    ErrorSource source = ErrorSource::Model;
    bool empty_answer = false;
    std::string content = job.text;
    std::string answer;
    std::vector<SearchResult> sources;
    std::string finish_reason;
    std::size_t dropped = 0;
    try {
        if (job.query.has_value()) {
            source = ErrorSource::Search;
            const std::unique_ptr<SearchProvider> provider =
                dependencies_.make_search ? dependencies_.make_search() : nullptr;
            const Result<SearchResponse> found =
                provider ? provider->search(*job.query, &cancel_)
                         : Result<SearchResponse>{ChatError{
                               ErrorKind::Config, 0, "no se pudo crear el proveedor de búsqueda",
                               std::nullopt}};
            if (found.is_error()) {
                failure = found.error();
            } else if (found.value().results.empty()) {
                failure = ChatError{ErrorKind::NoSearchResults, 0, std::string{kNoSearchResults},
                                    std::nullopt};
            } else {
                // Como en la app: los resultados son datos dentro del bloque
                // con nonce, que reemplaza al texto del comando.
                const SearchResponse trimmed = trim_search_response(found.value());
                const std::string date = local_iso_date(
                    std::chrono::system_clock::to_time_t(dependencies_.clock.now()));
                content = format_search_context(trimmed, spanish_date(date), generate_search_nonce());
                sources = trimmed.results;
            }
        }
        if (!failure.has_value()) {
            source = ErrorSource::Model;
            const std::unique_ptr<ChatClient> client =
                dependencies_.make_client ? dependencies_.make_client() : nullptr;
            if (!client) {
                failure = ChatError{ErrorKind::Config, 0, "no se pudo crear el cliente del modelo",
                                    std::nullopt};
            } else {
                std::vector<Message> request = job.history;
                request.push_back(Message{Role::User, content});
                TrimmedCompletion done = complete_trimmed(
                    *client, request, config_.history_limit_bytes,
                    [&answer](std::string_view delta) {
                        answer += delta;
                        return true;
                    },
                    &cancel_);
                dropped = done.dropped;
                if (done.result.is_error()) {
                    failure = done.result.error();
                } else if (is_blank(answer)) {
                    empty_answer = true;
                } else {
                    finish_reason = done.result.value().finish_reason;
                }
            }
        }
    } catch (...) {
        failure = ChatError{ErrorKind::BadResponse, 0, "excepción inesperada al responder",
                            std::nullopt};
    }
    try {
        outbox.set_typing(job.chat, false);
    } catch (...) {
    }

    if (failure.has_value() && source == ErrorSource::Search) {
        // El modelo no se llamó: el mensaje no cuenta para el tope diario.
        const std::lock_guard<std::mutex> lock(mutex_);
        counter_.refund(job.user, job.day);
    }
    if (failure.has_value() && failure->kind == ErrorKind::Cancelled && cancel_.is_cancelled()) {
        // El bot se está apagando: no se manda nada.
        log(LogLevel::Info, "cancelada " + ids + " ms=" + std::to_string(elapsed_ms(started)));
        release(job.chat, std::nullopt, std::nullopt);
        return;
    }
    if (failure.has_value() || empty_answer) {
        if (failure.has_value()) {
            log(failure->kind == ErrorKind::NoSearchResults ? LogLevel::Info : LogLevel::Error,
                "error " + ids +
                    " origen=" + (source == ErrorSource::Search ? "busqueda" : "modelo") +
                    " tipo=\"" + error_kind_label(failure->kind) +
                    "\" http=" + std::to_string(failure->http_status) +
                    " ms=" + std::to_string(elapsed_ms(started)) + " detalle=\"" +
                    one_line(failure->message, kMaxLogDetail) + "\"");
        } else {
            log(LogLevel::Error, "error " + ids + " origen=modelo tipo=\"respuesta vacía\" ms=" +
                                     std::to_string(elapsed_ms(started)));
        }
        const std::string text = failure.has_value() ? user_error_message(*failure, source)
                                                     : std::string{kEmptyAnswer};
        (void)deliver(job.chat, text, TextFormat::Plain);
        release(job.chat, std::nullopt, std::nullopt);
        return;
    }

    // El par entra al historial antes de mandarlo; el chat sigue ocupado
    // hasta mandar todo, para que la siguiente respuesta no se intercale.
    release(job.chat, content, answer);
    const std::optional<std::size_t> pieces = [&]() -> std::optional<std::size_t> {
        std::optional<std::size_t> sent = deliver(job.chat, answer, TextFormat::Markdown);
        if (sent.has_value() && !sources.empty()) {
            (void)deliver(job.chat, sources_text(sources), TextFormat::Plain);
        }
        if (const std::string note = finish_note(finish_reason); sent.has_value() && !note.empty()) {
            (void)deliver(job.chat, note, TextFormat::Plain);
        }
        return sent;
    }();
    log(LogLevel::Info, "respuesta " + ids + " ms=" + std::to_string(elapsed_ms(started)) +
                            " bytes=" + std::to_string(answer.size()) +
                            " pedazos=" + (pieces.has_value() ? std::to_string(*pieces) : "-") +
                            " recortados=" + std::to_string(dropped) +
                            " fin=" + one_line(finish_reason.empty() ? "-" : finish_reason, 32));
    release(job.chat, std::nullopt, std::nullopt);
}

void Bot::release(ChatId chat, std::optional<std::string> user_content,
                  std::optional<std::string> answer) {
    const TimePoint now = dependencies_.clock.now();
    const std::lock_guard<std::mutex> lock(mutex_);
    Session* session = sessions_.find(chat);
    if (session == nullptr) {
        return;
    }
    if (user_content.has_value() && answer.has_value()) {
        session->commit(std::move(*user_content), std::move(*answer));
        return; // Sigue ocupada hasta mandar la respuesta.
    }
    session->busy = false;
    session->last_activity = now;
}

std::optional<std::size_t> Bot::deliver(ChatId chat, std::string_view text, TextFormat format) {
    const std::vector<std::string> pieces =
        format == TextFormat::Markdown ? split_markdown(text, limit_) : split_plain(text, limit_);
    Outbox& outbox = dependencies_.outbox;
    const auto send = [&](const std::string& piece, TextFormat as) {
        try {
            return outbox.send(chat, piece, as);
        } catch (...) {
            return false;
        }
    };
    for (std::size_t i = 0; i < pieces.size(); ++i) {
        if (cancel_.is_cancelled()) {
            // Apagando: lo que falta no se manda, para que shutdown() no
            // espere a la plataforma.
            log(LogLevel::Info, "no_enviado_por_apagado chat=" + std::to_string(chat) +
                                    " pedazos=" + std::to_string(pieces.size() - i));
            return std::nullopt;
        }
        if (send(pieces[i], format)) {
            continue;
        }
        if (format == TextFormat::Markdown && send(pieces[i], TextFormat::Plain)) {
            log(LogLevel::Warning, "enviado_como_texto chat=" + std::to_string(chat) +
                                       " pedazo=" + std::to_string(i + 1) + "/" +
                                       std::to_string(pieces.size()));
            continue;
        }
        log(LogLevel::Error, "no_enviado chat=" + std::to_string(chat) + " pedazo=" +
                                 std::to_string(i + 1) + "/" + std::to_string(pieces.size()));
        return std::nullopt;
    }
    return pieces.size();
}

void Bot::expire_idle() {
    std::size_t expired = 0;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        expired = sessions_.expire(dependencies_.clock.now());
    }
    if (expired > 0) {
        log(LogLevel::Info, "sesiones_borradas n=" + std::to_string(expired));
    }
}

std::size_t Bot::session_count() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return sessions_.size();
}

bool Bot::wait_idle(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return idle_.wait_for(lock, timeout, [this] { return queue_.empty() && running_ == 0; });
}

void Bot::shutdown() {
    // Una llamada que llega mientras otra apaga espera a que termine:
    // después de shutdown() nunca quedan hilos.
    std::call_once(shutdown_once_, [this] {
        std::size_t discarded = 0;
        std::size_t running = 0;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
            discarded = queue_.size();
            for (const Job& job : queue_) {
                if (Session* session = sessions_.find(job.chat)) {
                    session->busy = false;
                }
            }
            queue_.clear();
            running = running_;
        }
        cancel_.cancel();
        work_ready_.notify_all();
        for (std::thread& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        workers_.clear();
        idle_.notify_all();
        log(LogLevel::Info, "apagado en_curso=" + std::to_string(running) +
                                " descartadas=" + std::to_string(discarded));
    });
}

} // namespace chatbot::bot
