// chatbot-telegram-probe: sonda manual (como tools/smoke). Manda a un chat
// unos mensajes enriquecidos de prueba para ver en el celular cómo los
// muestra Telegram: el signo de pesos, las fórmulas, los escapes con '\' y
// con entidades, y una vista previa con streaming. Nunca corre en las
// pruebas ni en la CI: solo a mano, con el token de un bot de prueba.
//
//   CHAT_TELEGRAM_TOKEN=... ./chatbot-telegram-probe --chat <tu ID>
//
// Tu ID: escríbele al bot mientras corre chatbot-telegram sin autorizarte;
// te contesta con él.

#include "chatbot/telegram/api.h"
#include "chatbot/telegram/rich_markdown.h"

#include "chatbot/addon_settings/token.h"

#include "chatbot/curl_transport.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using namespace chatbot;
using namespace chatbot::telegram;
using namespace std::chrono_literals;

struct Case {
    const char* description;
    std::string markdown;
    bool sanitize; ///< true: pasa por to_rich_markdown (lo que hace el bot).
};

std::vector<Case> cases() {
    const std::string sample = "# Muestra del bot\n\n"
                               "Texto con **negritas**, *cursiva*, ~~tachado~~, ==marcado== y "
                               "`código`.\nSegunda línea del párrafo.\n\n"
                               "- uno\n- dos\n  - anidado\n\n1. primero\n2. segundo\n\n"
                               "- [ ] pendiente\n- [x] hecho\n\n> Una cita.\n\n"
                               "> [!TIP]\n> Una alerta (llega como cita con título).\n\n"
                               "| Materia | Calificación |\n|:--|--:|\n| Física | 9 |\n"
                               "| Química | 10 |\n\n"
                               "```python\ndef hola():\n\tprint(\"<b>tab</b>\")\n```\n\n"
                               "Con nota[^1] y un [enlace](https://telegram.org).\n\n"
                               "[^1]: La nota al pie.";
    // Cada escape con '\\' que manda el bot, en su propia línea, para ver
    // cuál deja la barra visible. "\\\\" es una barra escapada: debe verse
    // una sola.
    const std::string escapes = "1 \\*asterisco\\*\n"
                                "2 \\_guion bajo\\_\n"
                                "3 \\~tilde\\~\n"
                                "4 \\|\\|barras\\|\\|\n"
                                "5 \\=\\=iguales\\=\\=\n"
                                "6 \\`backtick\\`\n"
                                "7 \\[corchetes\\]\n"
                                "8 mayor que \\>\n"
                                "9 pesos \\$5\n"
                                "10 una barra: \\\\\n"
                                "11 admiración al final\\!\n"
                                "\\# 12 gato al principio\n"
                                "\\- 13 guion al principio\n"
                                "\\+ 14 más al principio\n"
                                "15\\. número al principio\n"
                                "\\> 16 mayor al principio";
    return {
        {"pesos como los manda el bot", "Cuesta $50 y la otra $60.", true},
        {"fórmula en línea", "La suma: $x^2 + y^2$ y listo.", true},
        {"fórmula en bloque", "$$E = mc^2$$", true},
        {"fórmula con < (\\lt)", "Si $a<b$ y $c>d$.", true},
        {"HTML como lo manda el bot (ahora con &lt;): ninguna etiqueta ni botón",
         "<b>no negritas</b> <tg-button type=\"url\" url=\"https://t.me\">no botón</tg-button>",
         true},
        {"& como lo manda el bot (&amp;). Debe verse: Tom & Jerry, AT&T y la entidad escrita "
         "&lt;",
         "Tom & Jerry, AT&T y la entidad escrita &amp;lt;", true},
        {"cada escape con \\ (anota cuáles dejan ver la barra)", escapes, false},
        {"imagen como la manda el bot (un enlace)",
         "Logo: ![logo de Telegram](https://telegram.org/img/t_logo.png)", true},
        {"enlace tg:// como lo manda el bot (solo texto)", "[abrir](tg://resolve?domain=telegram)",
         true},
        {"muestra completa como la manda el bot", sample, true},
    };
}

/// El texto exacto que se manda, entre líneas, para compararlo con lo que
/// se ve en el chat.
void show(std::string_view text) {
    std::cout << "  manda (entre las líneas):\n  ----\n";
    std::size_t pos = 0;
    while (pos <= text.size()) {
        const std::size_t newline = text.find('\n', pos);
        const std::size_t end = newline == std::string_view::npos ? text.size() : newline;
        std::cout << "  " << text.substr(pos, end - pos) << "\n";
        if (newline == std::string_view::npos) {
            break;
        }
        pos = newline + 1;
    }
    std::cout << "  ----\n";
}

void report(const char* what, const std::optional<ChatError>& error) {
    if (error.has_value()) {
        std::cout << "  " << what << ": ERROR " << error->http_status << " " << error->message
                  << "\n";
    } else {
        std::cout << "  " << what << ": ok\n";
    }
}

} // namespace

int main(int argc, char** argv) {
    std::int64_t chat_id = 0; // 0: no se dio un chat válido.
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string_view{argv[i]} == "--chat") {
            try {
                chat_id = std::stoll(argv[i + 1]);
            } catch (const std::exception&) {
                chat_id = 0;
            }
        }
    }
    const char* token = std::getenv("CHAT_TELEGRAM_TOKEN");
    if (chat_id == 0 || token == nullptr || !chatbot::addon_settings::plausible_token(token)) {
        std::cerr << "Uso: CHAT_TELEGRAM_TOKEN=<token> chatbot-telegram-probe --chat <tu ID>\n";
        return 1;
    }
    BotApi api{token, [] { return std::make_unique<CurlTransport>(); }};

    int number = 1;
    for (const Case& c : cases()) {
        const std::string markdown = c.sanitize ? to_rich_markdown(c.markdown) : c.markdown;
        const std::string title = "Caso " + std::to_string(number) + ": " + c.description;
        std::cout << title << "\n";
        show(markdown);
        report("título", api.send_message(chat_id, title));
        std::this_thread::sleep_for(1100ms);
        report("mensaje enriquecido", api.send_rich_message(chat_id, markdown));
        std::this_thread::sleep_for(1100ms);
        ++number;
    }

    // Vista previa: "pensando", tres actualizaciones y el mensaje final; y
    // una con un bloque de código sin cerrar (crudo).
    std::cout << "Caso " << number << ": vista previa con streaming\n";
    report("título", api.send_message(chat_id, "Caso " + std::to_string(number) +
                                                 ": vista previa (mira cómo cambia)"));
    const std::int64_t draft = 4242;
    const std::string thinking = "<tg-thinking>Pensando…</tg-thinking>";
    show(thinking);
    report("pensando", api.send_rich_message_draft(chat_id, draft, thinking));
    std::this_thread::sleep_for(2s);
    std::string text;
    for (const char* part : {"Primera parte de la respuesta. ", "Segunda parte, con **negritas**. ",
                             "Tercera parte y `código`."}) {
        text += part;
        const std::string rich = to_rich_markdown(text);
        show(rich);
        report("borrador", api.send_rich_message_draft(chat_id, draft, rich));
        std::this_thread::sleep_for(1500ms);
    }
    const std::string unclosed = text + "\n\n```python\nprint(1)";
    show(unclosed);
    report("bloque sin cerrar (crudo)", api.send_rich_message_draft(chat_id, draft, unclosed));
    std::this_thread::sleep_for(1500ms);
    const std::string final_text = to_rich_markdown(text);
    show(final_text);
    report("mensaje final", api.send_rich_message(chat_id, final_text));
    std::cout << "Listo: revisa el chat y anota qué se vio en cada caso.\n";
    return 0;
}
