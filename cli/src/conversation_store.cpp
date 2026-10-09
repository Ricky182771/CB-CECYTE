#include "conversation_store.h"

#include <nlohmann/json.hpp>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <system_error>
#include <utility>

namespace chatbot::cli {
namespace {

using nlohmann::json;
namespace fs = std::filesystem;

/// Descripción de errno según la libc.
std::string errno_text(int error) {
    return std::string{std::strerror(error)};
}

/// Un id válido solo tiene dígitos, a-f y '-': no puede salir de la carpeta.
bool is_valid_id(const std::string& id) {
    if (id.empty() || id.size() > 64) {
        return false;
    }
    return std::all_of(id.begin(), id.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || c == '-';
    });
}

std::string role_name(Role role) {
    return role == Role::Assistant ? "assistant" : "user";
}

/// Lee el archivo completo. nullopt si no se puede abrir.
std::optional<std::string> read_whole(const fs::path& path) {
    std::ifstream file{path, std::ios::binary};
    if (!file.is_open()) {
        return std::nullopt;
    }
    std::string content{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
    if (file.bad()) {
        return std::nullopt;
    }
    return content;
}

/// Lee una cadena opcional del objeto; vacía si falta. Error si no es cadena.
bool optional_string(const json& object, const char* key, std::string& out, std::string& error) {
    const auto it = object.find(key);
    if (it == object.end() || it->is_null()) {
        out.clear();
        return true;
    }
    if (!it->is_string()) {
        error = std::string{"el campo \""} + key + "\" no es una cadena";
        return false;
    }
    out = it->get<std::string>();
    return true;
}

/// Lee la llave opcional "search" de un mensaje. Devuelve el error en
/// español o nullopt (también si no está).
std::optional<std::string> parse_search(const json& item, std::optional<StoredSearch>& out) {
    out.reset();
    const auto search = item.find("search");
    if (search == item.end() || search->is_null()) {
        return std::nullopt;
    }
    if (!search->is_object()) {
        return "la búsqueda de un mensaje no es un objeto";
    }
    StoredSearch stored;
    std::string error;
    if (!optional_string(*search, "query", stored.response.query, error) ||
        !optional_string(*search, "date", stored.date, error)) {
        return error;
    }
    const auto results = search->find("results");
    if (results == search->end() || !results->is_array()) {
        return "la búsqueda de un mensaje no tiene la lista \"results\"";
    }
    for (const json& result : *results) {
        if (!result.is_object()) {
            return "un resultado de búsqueda no es un objeto";
        }
        SearchResult parsed;
        if (!optional_string(result, "title", parsed.title, error) ||
            !optional_string(result, "url", parsed.url, error) ||
            !optional_string(result, "content", parsed.content, error) ||
            !optional_string(result, "published_date", parsed.published_date, error)) {
            return error;
        }
        stored.response.results.push_back(std::move(parsed));
    }
    out = std::move(stored);
    return std::nullopt;
}

/// Interpreta el contenido de un archivo. Devuelve el error en español o
/// nullopt si es válido.
std::optional<std::string> parse_conversation(const std::string& content,
                                              StoredConversation& out) {
    json document;
    try {
        document = json::parse(content);
    } catch (const json::exception&) {
        return "el archivo no es JSON válido";
    }
    if (!document.is_object()) {
        return "el archivo no es un objeto JSON";
    }
    const auto version = document.find("version");
    if (version == document.end() || !version->is_number_integer()) {
        return "no tiene \"version\"";
    }
    if (version->get<long long>() != kConversationFormatVersion) {
        return "versión " + std::to_string(version->get<long long>()) +
               " desconocida (este programa entiende la " +
               std::to_string(kConversationFormatVersion) + ")";
    }
    std::string error;
    for (const auto& [key, field] :
         std::array<std::pair<const char*, std::string*>, 4>{{{"id", &out.id},
                                                              {"title", &out.title},
                                                              {"created_at", &out.created_at},
                                                              {"updated_at", &out.updated_at}}}) {
        if (!optional_string(document, key, *field, error)) {
            return error;
        }
    }
    if (out.id.empty()) {
        return "no tiene \"id\"";
    }
    const auto messages = document.find("messages");
    if (messages == document.end() || !messages->is_array()) {
        return "no tiene la lista \"messages\"";
    }
    out.messages.clear();
    for (const json& item : *messages) {
        if (!item.is_object()) {
            return "un mensaje no es un objeto";
        }
        StoredMessage message;
        std::string role;
        if (!optional_string(item, "role", role, error) ||
            !optional_string(item, "content", message.content, error) ||
            !optional_string(item, "model", message.model, error) ||
            !optional_string(item, "finish_reason", message.finish_reason, error)) {
            return error;
        }
        if (const std::optional<std::string> search_error = parse_search(item, message.search)) {
            return search_error;
        }
        if (role == "user") {
            message.role = Role::User;
        } else if (role == "assistant") {
            message.role = Role::Assistant;
        } else {
            return "un mensaje tiene un rol desconocido";
        }
        out.messages.push_back(std::move(message));
    }
    return std::nullopt;
}

/// Se escribe con ordered_json para que las llaves sigan el orden del
/// esquema (más fácil de leer a mano); requiere nlohmann/json ≥ 3.9.
nlohmann::ordered_json to_json(const StoredConversation& conversation) {
    using nlohmann::ordered_json;
    ordered_json messages = ordered_json::array();
    for (const StoredMessage& message : conversation.messages) {
        ordered_json item = ordered_json::object();
        item["role"] = role_name(message.role);
        item["content"] = message.content;
        if (message.role == Role::Assistant) {
            item["model"] = message.model;
            item["finish_reason"] = message.finish_reason;
        } else if (message.search.has_value()) {
            ordered_json results = ordered_json::array();
            for (const SearchResult& result : message.search->response.results) {
                ordered_json entry = ordered_json::object();
                entry["title"] = result.title;
                entry["url"] = result.url;
                entry["content"] = result.content;
                entry["published_date"] = result.published_date;
                results.push_back(std::move(entry));
            }
            ordered_json search = ordered_json::object();
            search["query"] = message.search->response.query;
            search["date"] = message.search->date;
            search["results"] = std::move(results);
            item["search"] = std::move(search);
        }
        messages.push_back(std::move(item));
    }
    ordered_json document = ordered_json::object();
    document["version"] = kConversationFormatVersion;
    document["id"] = conversation.id;
    document["title"] = conversation.title;
    document["created_at"] = conversation.created_at;
    document["updated_at"] = conversation.updated_at;
    document["messages"] = std::move(messages);
    return document;
}

/// Días desde 1970-01-01 para una fecha civil (algoritmo de H. Hinnant).
long long days_from_civil(long long year, unsigned month, unsigned day) {
    year -= month <= 2 ? 1 : 0;
    const long long era = (year >= 0 ? year : year - 399) / 400;
    const auto yoe = static_cast<unsigned>(year - era * 400);
    const unsigned shifted_month = month > 2 ? month - 3 : month + 9; // Marzo = 0.
    const unsigned doy = (153 * shifted_month + 2) / 5 + day - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<long long>(doe) - 719468;
}

/// "AAAA-MM-DDTHH:MM:SS±HH:MM" → segundos UTC. nullopt si no tiene ese formato.
std::optional<long long> iso8601_to_utc(const std::string& text) {
    if (text.size() != 25 || text[4] != '-' || text[7] != '-' || text[10] != 'T' ||
        text[13] != ':' || text[16] != ':' || (text[19] != '+' && text[19] != '-') ||
        text[22] != ':') {
        return std::nullopt;
    }
    const auto number = [&text](std::size_t at, std::size_t length) -> std::optional<long long> {
        long long value = 0;
        for (std::size_t i = at; i < at + length; ++i) {
            if (text[i] < '0' || text[i] > '9') {
                return std::nullopt;
            }
            value = value * 10 + (text[i] - '0');
        }
        return value;
    };
    const auto year = number(0, 4);
    const auto month = number(5, 2);
    const auto day = number(8, 2);
    const auto hour = number(11, 2);
    const auto minute = number(14, 2);
    const auto second = number(17, 2);
    const auto offset_hour = number(20, 2);
    const auto offset_minute = number(23, 2);
    if (!year || !month || !day || !hour || !minute || !second || !offset_hour ||
        !offset_minute || *month < 1 || *month > 12 || *day < 1 || *day > 31) {
        return std::nullopt;
    }
    const long long days =
        days_from_civil(*year, static_cast<unsigned>(*month), static_cast<unsigned>(*day));
    const long long offset = (*offset_hour * 3600 + *offset_minute * 60) * (text[19] == '-' ? -1 : 1);
    return days * 86400 + *hour * 3600 + *minute * 60 + *second - offset;
}

/// Escribe todo el búfer en fd, reintentando escrituras parciales.
bool write_all(int fd, const std::string& data) {
    std::size_t written = 0;
    while (written < data.size()) {
        const ssize_t result = ::write(fd, data.data() + written, data.size() - written);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        written += static_cast<std::size_t>(result);
    }
    return true;
}

/// Crea el directorio con 0700 (y sus padres con los permisos por defecto).
std::optional<std::string> ensure_directory(const fs::path& dir) {
    std::error_code error;
    if (fs::is_directory(dir, error)) {
        return std::nullopt;
    }
    if (dir.has_parent_path()) {
        fs::create_directories(dir.parent_path(), error);
        if (error) {
            return "no se pudo crear " + dir.parent_path().string() + ": " + error.message();
        }
    }
    if (::mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) {
        return "no se pudo crear " + dir.string() + ": " + errno_text(errno);
    }
    return std::nullopt;
}

} // namespace

std::optional<std::string> resolve_data_dir(const std::optional<std::string>& chat_data_dir,
                                            const std::optional<std::string>& xdg_data_home,
                                            const std::optional<std::string>& home) {
    if (chat_data_dir.has_value() && !chat_data_dir->empty()) {
        return *chat_data_dir;
    }
    if (xdg_data_home.has_value() && !xdg_data_home->empty() && xdg_data_home->front() == '/') {
        std::string base = *xdg_data_home;
        while (base.size() > 1 && base.back() == '/') {
            base.pop_back();
        }
        return base + "/chatbot/conversations";
    }
    if (home.has_value() && !home->empty()) {
        return *home + "/.local/share/chatbot/conversations";
    }
    return std::nullopt;
}

std::string make_title(std::string_view first_user_message) {
    // Colapsa cualquier secuencia de espacios en blanco a un solo espacio.
    std::string collapsed;
    bool pending_space = false;
    for (const char c : first_user_message) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f') {
            pending_space = !collapsed.empty();
            continue;
        }
        if (pending_space) {
            collapsed.push_back(' ');
            pending_space = false;
        }
        collapsed.push_back(c);
    }
    // Cuenta puntos de código: los bytes de continuación (10xxxxxx) no cuentan.
    std::size_t code_points = 0;
    for (std::size_t i = 0; i < collapsed.size(); ++i) {
        const auto byte = static_cast<unsigned char>(collapsed[i]);
        if ((byte & 0xC0U) != 0x80U) {
            if (code_points == kTitleMaxCodePoints) {
                return collapsed.substr(0, i) + "…";
            }
            ++code_points;
        }
    }
    return collapsed;
}

std::string format_iso8601(std::time_t time) {
    std::tm local{};
    if (localtime_r(&time, &local) == nullptr) {
        return {};
    }
    std::array<char, 32> buffer{};
    if (std::strftime(buffer.data(), buffer.size(), "%Y-%m-%dT%H:%M:%S%z", &local) == 0) {
        return {};
    }
    std::string text{buffer.data()};
    // %z da "-0600"; ISO 8601 extendido lleva "-06:00".
    if (text.size() == 24) {
        text.insert(22, ":");
    }
    return text;
}

std::string format_conversation_id(std::time_t time, std::uint32_t random) {
    std::tm local{};
    if (localtime_r(&time, &local) == nullptr) {
        local = std::tm{};
    }
    std::array<char, 32> stamp{};
    std::strftime(stamp.data(), stamp.size(), "%Y%m%d-%H%M%S", &local);
    std::array<char, 8> hex{};
    std::snprintf(hex.data(), hex.size(), "%06x", static_cast<unsigned>(random & 0xFFFFFFU));
    return std::string{stamp.data()} + "-" + hex.data();
}

ConversationStore::ConversationStore(std::string dir) : dir_(std::move(dir)) {}

std::string ConversationStore::path_for(const std::string& id) const {
    return (fs::path{dir_} / (id + ".json")).string();
}

std::string ConversationStore::new_id() const {
    try {
        std::random_device device;
        std::uniform_int_distribution<std::uint32_t> distribution(0, 0xFFFFFFU);
        for (;;) {
            std::string id = format_conversation_id(std::time(nullptr), distribution(device));
            std::error_code error;
            if (dir_.empty() || !fs::exists(path_for(id), error)) {
                return id;
            }
        }
    } catch (...) {
        // random_device puede lanzar si no hay fuente de entropía: se usa el
        // reloj de alta resolución como respaldo.
        const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
        return format_conversation_id(std::time(nullptr), static_cast<std::uint32_t>(ticks));
    }
}

std::vector<ConversationSummary> ConversationStore::list() const {
    std::vector<ConversationSummary> result;
    std::vector<long long> sort_keys;
    if (dir_.empty()) {
        return result;
    }
    try {
        std::error_code error;
        fs::directory_iterator it{dir_, error};
        if (error) {
            return result; // Sin directorio todavía: no hay conversaciones.
        }
        for (const fs::directory_entry& entry : it) {
            const fs::path& path = entry.path();
            if (path.extension() != ".json") {
                continue; // Ignora *.tmp y cualquier otra cosa.
            }
            ConversationSummary summary;
            summary.id = path.stem().string();
            long long key = 0;
            StoredConversation conversation;
            const std::optional<std::string> content = read_whole(path);
            std::optional<std::string> problem =
                content.has_value() ? parse_conversation(*content, conversation)
                                    : std::optional<std::string>{"no se pudo leer el archivo"};
            if (!problem.has_value() && conversation.id != summary.id) {
                problem = "el id del archivo no coincide con su nombre";
            }
            if (problem.has_value()) {
                summary.readable = false;
                summary.error = *problem;
                summary.title = path.filename().string();
            } else {
                summary.title = conversation.title;
                summary.updated_at = conversation.updated_at;
                summary.message_count = conversation.messages.size();
                for (auto message = conversation.messages.rbegin();
                     message != conversation.messages.rend(); ++message) {
                    if (message->role == Role::Assistant && !message->model.empty()) {
                        summary.last_model = message->model;
                        break;
                    }
                }
                key = iso8601_to_utc(conversation.updated_at).value_or(0);
            }
            result.push_back(std::move(summary));
            sort_keys.push_back(key);
        }
    } catch (...) {
        // Un fallo al recorrer el directorio deja la lista como esté.
    }

    // Legibles primero, de la más reciente a la más vieja; ilegibles al final.
    std::vector<std::size_t> order(result.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
        order[i] = i;
    }
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        if (result[a].readable != result[b].readable) {
            return result[a].readable;
        }
        if (sort_keys[a] != sort_keys[b]) {
            return sort_keys[a] > sort_keys[b];
        }
        return result[a].id > result[b].id;
    });
    std::vector<ConversationSummary> sorted;
    sorted.reserve(result.size());
    for (const std::size_t index : order) {
        sorted.push_back(std::move(result[index]));
    }
    return sorted;
}

LoadResult ConversationStore::load(const std::string& id) const {
    if (dir_.empty()) {
        return {std::nullopt, "No se pudo determinar la carpeta de conversaciones."};
    }
    if (!is_valid_id(id)) {
        return {std::nullopt, "Identificador de conversación inválido."};
    }
    try {
        const std::optional<std::string> content = read_whole(path_for(id));
        if (!content.has_value()) {
            return {std::nullopt, "No se pudo leer la conversación."};
        }
        StoredConversation conversation;
        if (const std::optional<std::string> problem = parse_conversation(*content, conversation)) {
            return {std::nullopt, "La conversación es ilegible: " + *problem + "."};
        }
        return {std::move(conversation), {}};
    } catch (...) {
        return {std::nullopt, "No se pudo leer la conversación."};
    }
}

std::optional<std::string> ConversationStore::save(const StoredConversation& conversation) const {
    if (dir_.empty()) {
        return "no se pudo determinar la carpeta de conversaciones";
    }
    if (!is_valid_id(conversation.id)) {
        return "identificador de conversación inválido";
    }
    try {
        const std::string target = path_for(conversation.id);
        // Nunca se sobrescribe un archivo ilegible: podría ser de una versión futura.
        if (const std::optional<std::string> existing = read_whole(target)) {
            StoredConversation ignored;
            if (const std::optional<std::string> problem = parse_conversation(*existing, ignored)) {
                return "el archivo existente es ilegible (" + *problem + ") y no se sobrescribe";
            }
        }
        if (const std::optional<std::string> problem = ensure_directory(fs::path{dir_})) {
            return problem;
        }

        const std::string data =
            to_json(conversation)
                .dump(2, ' ', false, nlohmann::ordered_json::error_handler_t::replace) +
            "\n";
        const std::string temporary = target + ".tmp";
        const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (fd < 0) {
            return "no se pudo crear " + temporary + ": " + errno_text(errno);
        }
        // Si el .tmp ya existía con otros permisos, O_TRUNC los conserva.
        const bool written = ::fchmod(fd, 0600) == 0 && write_all(fd, data) && ::fsync(fd) == 0;
        const int write_error = errno;
        if (::close(fd) != 0 || !written) {
            ::unlink(temporary.c_str());
            return "no se pudo escribir " + temporary + ": " + errno_text(write_error);
        }
        if (::rename(temporary.c_str(), target.c_str()) != 0) {
            const int rename_error = errno;
            ::unlink(temporary.c_str());
            return "no se pudo reemplazar " + target + ": " + errno_text(rename_error);
        }
        // fsync del directorio: que el rename sobreviva a un apagón.
        const int dir_fd = ::open(dir_.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (dir_fd >= 0) {
            ::fsync(dir_fd);
            ::close(dir_fd);
        }
        return std::nullopt;
    } catch (...) {
        return "error inesperado al guardar";
    }
}

std::optional<std::string> ConversationStore::remove(const std::string& id) const {
    if (dir_.empty()) {
        return "no se pudo determinar la carpeta de conversaciones";
    }
    if (!is_valid_id(id)) {
        return "identificador de conversación inválido";
    }
    try {
        const std::string target = path_for(id);
        const std::optional<std::string> content = read_whole(target);
        if (!content.has_value()) {
            return "la conversación no existe";
        }
        StoredConversation ignored;
        if (const std::optional<std::string> problem = parse_conversation(*content, ignored)) {
            return "el archivo es ilegible (" + *problem + ") y no se borra";
        }
        if (::unlink(target.c_str()) != 0) {
            return "no se pudo borrar " + target + ": " + errno_text(errno);
        }
        return std::nullopt;
    } catch (...) {
        return "error inesperado al borrar";
    }
}

} // namespace chatbot::cli
