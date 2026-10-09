#include "chatbot/curl_transport.h"

#include "chatbot/cancel_token.h"
#include "chatbot/platform.h"

#include <curl/curl.h>

#include <cctype>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace chatbot {
namespace {

/// Inicialización global de curl exactamente una vez, con limpieza al salir
/// del proceso (RAII, sección 8).
class GlobalCurlInit {
public:
    GlobalCurlInit() { curl_global_init(CURL_GLOBAL_DEFAULT); }
    ~GlobalCurlInit() { curl_global_cleanup(); }
};

void ensure_curl_global_init() {
    static const GlobalCurlInit guard;
}

struct CurlHandleDeleter {
    void operator()(CURL* handle) const { curl_easy_cleanup(handle); }
};

struct SlistDeleter {
    void operator()(curl_slist* list) const { curl_slist_free_all(list); }
};

/// Estado del modo streaming: acumula el cuerpo en la respuesta y avisa al
/// observador conforme llegan los datos, con el estado HTTP vigente.
struct StreamState {
    std::string* body = nullptr; ///< Siempre apunta a HttpResponse::body.
    const StreamCallback* on_chunk = nullptr;
    CURL* handle = nullptr;      ///< Para consultar el estado HTTP en curso.
    bool cancelled = false;
};

/// Estado del callback de progreso: revisa el token de cancelación.
struct ProgressState {
    const CancelToken* cancel = nullptr;
    bool aborted = false; ///< El callback abortó por cancelación.
};

/// Callback de progreso de libcurl (CURLOPT_XFERINFOFUNCTION). Se llama con
/// frecuencia mientras hay datos y alrededor de una vez por segundo cuando no
/// llegan. Devolver distinto de 0 aborta con CURLE_ABORTED_BY_CALLBACK.
/// Frontera con C: no puede lanzar (is_cancelled es noexcept).
int progress_callback(void* userdata, curl_off_t /*dltotal*/, curl_off_t /*dlnow*/,
                      curl_off_t /*ultotal*/, curl_off_t /*ulnow*/) noexcept {
    auto* state = static_cast<ProgressState*>(userdata);
    if (state->cancel->is_cancelled()) {
        state->aborted = true;
        return 1;
    }
    return 0;
}

/// Quita espacios y saltos de línea de los extremos.
std::string trim(std::string text) {
    const auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
    std::size_t begin = 0;
    while (begin < text.size() && is_space(static_cast<unsigned char>(text[begin]))) {
        ++begin;
    }
    std::size_t end = text.size();
    while (end > begin && is_space(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }
    return text.substr(begin, end - begin);
}

/// Comparación de cadenas sin distinguir mayúsculas (para nombres de cabecera).
bool equals_case_insensitive(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t i = 0; i < left.size(); ++i) {
        const auto lc = static_cast<unsigned char>(left[i]);
        const auto rc = static_cast<unsigned char>(right[i]);
        if (std::tolower(lc) != std::tolower(rc)) {
            return false;
        }
    }
    return true;
}

/// Fallos de curl que no se arreglan reintentando: URL o protocolo inválidos
/// y problemas de certificados. DNS y conexión sí se reintentan.
bool is_retryable_curl_error(CURLcode code) {
    switch (code) {
    case CURLE_URL_MALFORMAT:
    case CURLE_UNSUPPORTED_PROTOCOL:
    case CURLE_PEER_FAILED_VERIFICATION:
    case CURLE_SSL_CERTPROBLEM:
    case CURLE_SSL_CACERT_BADFILE:
        return false;
    default:
        return true;
    }
}

size_t write_body_callback(char* data, size_t size, size_t count, void* userdata) {
    auto* body = static_cast<std::string*>(userdata);
    body->append(data, size * count);
    return size * count;
}

size_t write_stream_callback(char* data, size_t size, size_t count, void* userdata) {
    auto* state = static_cast<StreamState*>(userdata);
    const size_t total = size * count;
    // Frontera con C: ninguna excepción puede salir de esta función.
    try {
        state->body->append(data, total);
        if (state->on_chunk != nullptr) {
            // Las cabeceras llegan antes que el cuerpo: el estado ya es el final.
            long status_code = 0;
            curl_easy_getinfo(state->handle, CURLINFO_RESPONSE_CODE, &status_code);
            if (!(*state->on_chunk)(std::string_view{data, total},
                                    static_cast<int>(status_code))) {
                state->cancelled = true;
                // Devolver 0 aborta la transferencia (curl la termina con error de escritura).
                return 0;
            }
        }
    } catch (...) {
        state->cancelled = true;
        return 0;
    }
    return total;
}

size_t header_callback(char* data, size_t size, size_t count, void* userdata) {
    auto* retry_after = static_cast<std::optional<std::string>*>(userdata);
    const size_t total = size * count;
    const std::string line{data, total};

    const size_t colon = line.find(':');
    if (colon != std::string::npos &&
        equals_case_insensitive(trim(line.substr(0, colon)), "retry-after")) {
        *retry_after = trim(line.substr(colon + 1));
    }
    return total;
}

} // namespace

struct CurlTransport::Impl {
    Impl() {
        ensure_curl_global_init();
        handle.reset(curl_easy_init());
    }

    std::unique_ptr<CURL, CurlHandleDeleter> handle;
};

CurlTransport::CurlTransport() : impl_(std::make_unique<Impl>()) {}

CurlTransport::~CurlTransport() = default;

HttpResponse CurlTransport::send(const HttpRequest& request) {
    return perform(request, nullptr);
}

HttpResponse CurlTransport::send_stream(const HttpRequest& request,
                                        const StreamCallback& on_chunk) {
    return perform(request, &on_chunk);
}

HttpResponse CurlTransport::perform(const HttpRequest& request, const StreamCallback* stream) {
    HttpResponse response;
    if (impl_->handle == nullptr) {
        response.error = "libcurl no pudo inicializarse";
        return response;
    }
    CURL* handle = impl_->handle.get();
    curl_easy_reset(handle);

    char error_buffer[CURL_ERROR_SIZE] = {};

    // Cabeceras. La key vive solo aquí y nunca se imprime ni se guarda (sección 9).
    std::unique_ptr<curl_slist, SlistDeleter> headers;
    const bool is_get = request.method == HttpMethod::Get;
    headers.reset(curl_slist_append(nullptr, is_get ? "Accept: application/json"
                                                    : "Content-Type: application/json"));
    if (!request.api_key.empty()) { // Sin key (servidor local): sin Authorization.
        const std::string authorization = "Authorization: Bearer " + request.api_key;
        headers.reset(curl_slist_append(headers.release(), authorization.c_str()));
    }

    CURLcode setup = CURLE_OK;
    const auto set = [&handle, &setup](CURLoption option, auto value) {
        if (setup == CURLE_OK) {
            setup = curl_easy_setopt(handle, option, value);
        }
    };

    set(CURLOPT_URL, request.url.c_str());
    if (is_get) {
        set(CURLOPT_HTTPGET, 1L); // Sin cuerpo.
    } else {
        set(CURLOPT_POST, 1L);
        set(CURLOPT_POSTFIELDS, request.body.c_str());
        set(CURLOPT_POSTFIELDSIZE, static_cast<long>(request.body.size()));
    }
    set(CURLOPT_HTTPHEADER, headers.get());
    if (stream != nullptr) {
        // Streaming: sin límite total, para no cortar respuestas largas que
        // siguen llegando. El timeout se aplica por inactividad: si la
        // velocidad cae por debajo de 1 byte/s durante ese tiempo, curl
        // aborta con CURLE_OPERATION_TIMEDOUT. Curl promedia la velocidad en
        // una ventana de unos 5 s, así que el corte llega unos segundos
        // después del timeout pedido. 0 = sin límite.
        set(CURLOPT_TIMEOUT_MS, 0L);
        const long long timeout_ms = request.timeout.count();
        if (timeout_ms > 0) {
            // Segundos redondeados hacia arriba; mínimo 1.
            const long idle_seconds = static_cast<long>((timeout_ms + 999) / 1000);
            set(CURLOPT_LOW_SPEED_LIMIT, 1L);
            set(CURLOPT_LOW_SPEED_TIME, idle_seconds);
        }
    } else {
        // Timeout total (0 = sin límite).
        set(CURLOPT_TIMEOUT_MS, static_cast<long>(request.timeout.count()));
    }
    // 10 s para establecer conexión (sección 8).
    set(CURLOPT_CONNECTTIMEOUT_MS, 10000L);
    // Windows: los certificados del almacén del sistema, sin repartir un
    // cacert.pem. La verificación TLS sigue siempre activa (sección 8); en
    // Linux no cambia nada.
    if (current_os() == Os::Windows) {
        set(CURLOPT_SSL_OPTIONS, static_cast<long>(CURLSSLOPT_NATIVE_CA));
    }
    set(CURLOPT_ERRORBUFFER, error_buffer);
    set(CURLOPT_NOSIGNAL, 1L);

    // Streaming o acumulación: el mismo handle sirve para ambos modos.
    StreamState stream_state;
    if (stream != nullptr) {
        stream_state.body = &response.body;
        stream_state.on_chunk = stream;
        stream_state.handle = handle;
        set(CURLOPT_WRITEFUNCTION, write_stream_callback);
        set(CURLOPT_WRITEDATA, &stream_state);
    } else {
        set(CURLOPT_WRITEFUNCTION, write_body_callback);
        set(CURLOPT_WRITEDATA, &response.body);
    }

    set(CURLOPT_HEADERFUNCTION, header_callback);
    set(CURLOPT_HEADERDATA, &response.retry_after);

    // Cancelación: el callback de progreso revisa el token (send y send_stream).
    ProgressState progress_state;
    if (request.cancel != nullptr) {
        progress_state.cancel = request.cancel;
        set(CURLOPT_NOPROGRESS, 0L);
        set(CURLOPT_XFERINFOFUNCTION, progress_callback);
        set(CURLOPT_XFERINFODATA, &progress_state);
    }

    if (setup != CURLE_OK) {
        response = HttpResponse{};
        response.error =
            std::string{"no se pudo preparar la petición: "} + curl_easy_strerror(setup);
        return response;
    }

    const CURLcode result = curl_easy_perform(handle);
    if (result != CURLE_OK) {
        // Descarta capturas parciales: sin respuesta completa no hay cuerpo válido.
        const bool was_cancelled =
            stream_state.cancelled ||
            (result == CURLE_ABORTED_BY_CALLBACK && progress_state.aborted);
        response = HttpResponse{};
        response.timed_out = (result == CURLE_OPERATION_TIMEDOUT);
        response.cancelled = was_cancelled;
        response.retryable = !was_cancelled && is_retryable_curl_error(result);
        response.error = error_buffer[0] != '\0' ? std::string{error_buffer}
                                                 : std::string{curl_easy_strerror(result)};
        return response;
    }

    long status_code = 0;
    curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status_code);
    response.status = static_cast<int>(status_code);
    return response;
}

} // namespace chatbot
