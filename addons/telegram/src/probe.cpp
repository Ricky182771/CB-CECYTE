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
    return {
        {"pesos sin escapar (crudo)", "Cuesta $50 y la otra $60.", false},
        {"pesos como los manda el bot", "Cuesta $50 y la otra $60.", true},
        {"pesos con \\$ (crudo)", "Cuesta \\$50 y la otra \\$60.", false},
        {"fórmula en línea", "La suma: $x^2 + y^2$ y listo.", true},
        {"fórmula en bloque", "$$E = mc^2$$", true},
        {"fórmula con < (\\lt)", "Si $a<b$ y $c>d$.", true},
        {"fórmula con \\sqrt[3]", "Raíz: $\\sqrt[3]{x}$.", true},
        {"HTML escapado con \\", "\\<b\\>esto no debe salir en negritas\\</b\\>", false},
        {"HTML escapado con entidades", "&lt;b&gt;esto no debe salir en negritas&lt;/b&gt;", false},
        {"HTML como lo manda el bot",
         "<b>no negritas</b> <tg-button type=\"url\" url=\"https://t.me\">no botón</tg-button>",
         true},
        {"HTML sin escapar (crudo: debe verse en negritas)", "<b>sí negritas</b>", false},
        {"cada escape con \\ (no debe verse ninguna \\)",
         "\\*a\\* \\_b\\_ \\~c\\~ \\|\\|d\\|\\| \\=\\=e\\=\\= \\`f\\` \\[g\\] \\# \\& \\! \\$ \\\\",
         false},
        {"imagen como la manda el bot (un enlace)",
         "Logo: ![logo de Telegram](https://telegram.org/img/t_logo.png)", true},
        {"enlace tg:// como lo manda el bot (solo texto)", "[abrir](tg://resolve?domain=telegram)",
         true},
        {"salto duro con \\ al final", "línea uno\\\nlínea dos", false},
        {"salto suave", "línea uno\nlínea dos", false},
        {"inicio de línea escapado", "1\\. no es lista\n\\- ni viñeta\n\\# ni título", false},
        {"muestra completa como la manda el bot", sample, true},
    };
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
    if (chat_id == 0 || token == nullptr || !plausible_token(token)) {
        std::cerr << "Uso: CHAT_TELEGRAM_TOKEN=<token> chatbot-telegram-probe --chat <tu ID>\n";
        return 1;
    }
    BotApi api{token, [] { return std::make_unique<CurlTransport>(); }};

    int number = 1;
    for (const Case& c : cases()) {
        const std::string markdown = c.sanitize ? to_rich_markdown(c.markdown) : c.markdown;
        const std::string title = "Caso " + std::to_string(number) + ": " + c.description;
        std::cout << title << "\n";
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
    report("pensando", api.send_rich_message_draft(
                           chat_id, draft, "<tg-thinking>Pensando…</tg-thinking>"));
    std::this_thread::sleep_for(2s);
    std::string text;
    for (const char* part : {"Primera parte de la respuesta. ", "Segunda parte, con **negritas**. ",
                             "Tercera parte y `código`."}) {
        text += part;
        report("borrador", api.send_rich_message_draft(chat_id, draft, to_rich_markdown(text)));
        std::this_thread::sleep_for(1500ms);
    }
    report("bloque sin cerrar (crudo)",
           api.send_rich_message_draft(chat_id, draft, text + "\n\n```python\nprint(1)"));
    std::this_thread::sleep_for(1500ms);
    report("mensaje final", api.send_rich_message(chat_id, to_rich_markdown(text)));
    std::cout << "Listo: revisa el chat y anota qué se vio en cada caso.\n";
    return 0;
}
