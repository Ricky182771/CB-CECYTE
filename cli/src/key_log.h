#ifndef CHATBOT_CLI_KEY_LOG_H
#define CHATBOT_CLI_KEY_LOG_H

#include "paste.h"

#include <chrono>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace chatbot::cli {

// Registro de teclas para diagnosticar (CHAT_DEBUG_KEYS=/ruta/archivo, solo
// por entorno): una línea por evento que llega a la interfaz. Sin FTXUI:
// main.cpp traduce cada ftxui::Event a KeyLogEvent. De un carácter solo se
// escribe la longitud, nunca el contenido (por ahí pasa la API key).

/// Un evento de la terminal, ya traducido.
struct KeyLogEvent {
    enum class Kind { Special, Character, Mouse, Custom };
    Kind kind = Kind::Special;
    /// Special: la secuencia (se escribe en hex). Character: el texto (solo
    /// se escribe su longitud en bytes).
    std::string_view input;
    /// Mouse: nombres del botón y del movimiento, y la posición.
    std::string_view button;
    std::string_view motion;
    int x = 0;
    int y = 0;
};

/// Estado de la interfaz al llegar el evento.
struct KeyLogState {
    bool settings_open = false;
    bool sidebar_focused = false;
    PasteTarget paste_target = PasteTarget::None;
    bool bracketed_paste_open = false;
    /// Con la configuración abierta: Focused() de cada campo (nombre, valor).
    std::vector<std::pair<std::string_view, bool>> focused_fields;
};

/// Bytes en hex separados por espacios ("1b 5b 32 7e").
[[nodiscard]] std::string hex_bytes(std::string_view bytes);
/// Nombre del destino ("none", "single-line", "multi-line").
[[nodiscard]] std::string_view paste_target_name(PasteTarget target);
/// Hora local con milisegundos: "HH:MM:SS.mmm".
[[nodiscard]] std::string key_log_time(std::chrono::system_clock::time_point now);

/// Línea de un evento (sin salto final). time: key_log_time.
[[nodiscard]] std::string format_key_event(std::string_view time, const KeyLogEvent& event,
                                           const KeyLogState& state);
/// Línea del resultado de un atajo de pegado. clipboard_bytes: lo que dio
/// read_native_clipboard (nullopt si no había texto). delivered: se insertó
/// algo.
[[nodiscard]] std::string format_paste_shortcut(std::string_view time,
                                                std::optional<std::size_t> clipboard_bytes,
                                                bool delivered);

/// El archivo del registro. Sin ruta, o si no se puede abrir o escribir, no
/// hace nada y nunca avisa. Se usa solo desde el hilo de la interfaz.
class KeyLog {
public:
    explicit KeyLog(const std::optional<std::string>& path);

    [[nodiscard]] bool enabled() const { return file_.is_open(); }
    /// Agrega la línea con su salto y la vacía al disco (sirve aunque el
    /// proceso termine de golpe).
    void write(std::string_view line) noexcept;

private:
    std::ofstream file_;
};

} // namespace chatbot::cli

#endif // CHATBOT_CLI_KEY_LOG_H
