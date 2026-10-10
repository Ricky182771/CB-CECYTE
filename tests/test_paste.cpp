#include <catch2/catch_test_macros.hpp>

#include "paste.h"

#include "chatbot/utf8.h"

#include <chrono>
#include <string>

using chatbot::cli::BracketedPaste;
using chatbot::cli::kMaxPasteBytes;
using chatbot::cli::PasteKey;
using chatbot::cli::PasteTarget;
using chatbot::cli::prepare_paste;
using chatbot::cli::sanitize_paste;

TEST_CASE("sanitize_paste convierte CRLF y CR en LF", "[paste]") {
    CHECK(sanitize_paste("a\r\nb\rc\nd", true).text == "a\nb\nc\nd");
    CHECK(sanitize_paste("a\r\r\nb", true).text == "a\n\nb");
    CHECK(sanitize_paste("fin\r", true).text == "fin\n");
}

TEST_CASE("sanitize_paste cambia los tabuladores por 4 espacios", "[paste]") {
    CHECK(sanitize_paste("\tx = 1;\n\t\ty", true).text == "    x = 1;\n        y");
}

TEST_CASE("sanitize_paste quita controles C0, DEL y C1", "[paste]") {
    SECTION("C0 salvo los saltos y el tabulador") {
        CHECK(sanitize_paste(std::string{"a\x01\x07\x08\x1B[31mb\x1F"}, true).text == "a[31mb");
        CHECK(sanitize_paste(std::string{"x\0y", 3}, true).text == "xy");
    }
    SECTION("DEL") {
        CHECK(sanitize_paste("a\x7F" "b", true).text == "ab");
    }
    SECTION("C1 (U+0080 a U+009F), pero no U+00A0") {
        CHECK(sanitize_paste("a\xC2\x80\xC2\x9B" "b\xC2\x9F", true).text == "ab");
        CHECK(sanitize_paste("a\xC2\xA0" "b", true).text == "a\xC2\xA0" "b");
    }
    SECTION("El texto normal queda igual") {
        const std::string text = "¿Qué tal? ñandú — 日本 😀 {x}";
        CHECK(sanitize_paste(text, true).text == text);
        CHECK_FALSE(sanitize_paste(text, true).truncated);
    }
}

TEST_CASE("sanitize_paste sustituye el UTF-8 inválido", "[paste]") {
    const std::string r{chatbot::utf8::kReplacement};
    SECTION("Un byte suelto") {
        CHECK(sanitize_paste("a\xFF" "b", true).text == "a" + r + "b");
    }
    SECTION("Una secuencia incompleta al final: un U+FFFD por byte") {
        CHECK(sanitize_paste("a\xE2\x82", true).text == "a" + r + r);
    }
    SECTION("Sobrelarga y sustitutos") {
        CHECK(sanitize_paste("\xC0\xAF", true).text == r + r);
        CHECK(sanitize_paste("\xED\xA0\x80", true).text == r + r + r);
    }
    SECTION("El resultado siempre es UTF-8 válido") {
        CHECK(chatbot::utf8::is_valid(sanitize_paste("\x80\xBF\xF8\xC3", true).text));
    }
}

TEST_CASE("sanitize_paste en una línea", "[paste]") {
    SECTION("Quita los saltos del final (una key copiada de una página)") {
        CHECK(sanitize_paste("nvapi-abc123\n", false).text == "nvapi-abc123");
        CHECK(sanitize_paste("nvapi-abc123\r\n\r\n", false).text == "nvapi-abc123");
    }
    SECTION("Quita también los del principio") {
        CHECK(sanitize_paste("\n\nabc", false).text == "abc");
    }
    SECTION("Los de en medio pasan a un espacio") {
        CHECK(sanitize_paste("uno\ndos\r\ntres", false).text == "uno dos tres");
        CHECK(sanitize_paste("a\n\nb", false).text == "a  b");
    }
    SECTION("Solo saltos: vacío") {
        CHECK(sanitize_paste("\n\r\n", false).text.empty());
    }
    SECTION("Con varias líneas se conservan") {
        CHECK(sanitize_paste("\nabc\n", true).text == "\nabc\n");
    }
}

TEST_CASE("sanitize_paste recorta a kMaxPasteBytes sin partir un carácter", "[paste]") {
    SECTION("Justo en el tope no recorta") {
        const std::string text(kMaxPasteBytes, 'a');
        const auto paste = sanitize_paste(text, true);
        CHECK(paste.text.size() == kMaxPasteBytes);
        CHECK_FALSE(paste.truncated);
    }
    SECTION("Un byte de más recorta") {
        const std::string text(kMaxPasteBytes + 1, 'a');
        const auto paste = sanitize_paste(text, true);
        CHECK(paste.text.size() == kMaxPasteBytes);
        CHECK(paste.truncated);
    }
    SECTION("Un carácter de 4 bytes que no cabe completo queda fuera") {
        // Faltan 2 bytes para el tope: el emoji (4 bytes) no cabe.
        const std::string text = std::string(kMaxPasteBytes - 2, 'a') + "\xF0\x9F\x98\x80" + "b";
        const auto paste = sanitize_paste(text, true);
        CHECK(paste.truncated);
        CHECK(paste.text.size() == kMaxPasteBytes - 2);
        CHECK(chatbot::utf8::is_valid(paste.text));
    }
    SECTION("Un tabulador que ya no cabe tampoco se parte") {
        const std::string text = std::string(kMaxPasteBytes - 3, 'a') + "\t";
        const auto paste = sanitize_paste(text, true);
        CHECK(paste.truncated);
        CHECK(paste.text.size() == kMaxPasteBytes - 3);
    }
}

TEST_CASE("paste_shortcut_notice: avisa sin destino, salvo con la barra", "[paste]") {
    using chatbot::cli::paste_shortcut_notice;
    // Una lista, un botón o la pregunta de descartar: aviso.
    CHECK(paste_shortcut_notice(false, PasteTarget::None) == chatbot::cli::kPasteNoTarget);
    // La barra lateral: se ignora sin aviso, como antes.
    CHECK_FALSE(paste_shortcut_notice(true, PasteTarget::None).has_value());
    // Con destino no hay aviso.
    CHECK_FALSE(paste_shortcut_notice(false, PasteTarget::SingleLine).has_value());
    CHECK_FALSE(paste_shortcut_notice(false, PasteTarget::MultiLine).has_value());
}

TEST_CASE("prepare_paste según el destino", "[paste]") {
    SECTION("Con la barra lateral, el pegado se ignora") {
        const PasteTarget target = chatbot::cli::paste_target(true, false, PasteTarget::None);
        CHECK(target == PasteTarget::None);
        CHECK_FALSE(prepare_paste("hola\nmundo", target).has_value());
        // Aunque la configuración esté abierta detrás.
        CHECK(chatbot::cli::paste_target(true, true, PasteTarget::MultiLine) ==
              PasteTarget::None);
    }
    SECTION("Sin barra ni configuración, la caja de la conversación (varias líneas)") {
        const PasteTarget target = chatbot::cli::paste_target(false, false, PasteTarget::None);
        CHECK(target == PasteTarget::MultiLine);
        CHECK(prepare_paste("a\r\nb", target)->text == "a\nb");
    }
    SECTION("Con la configuración, el campo que tiene el foco") {
        CHECK(chatbot::cli::paste_target(false, true, PasteTarget::SingleLine) ==
              PasteTarget::SingleLine);
        CHECK(prepare_paste("key\n", PasteTarget::SingleLine)->text == "key");
        CHECK(chatbot::cli::paste_target(false, true, PasteTarget::None) == PasteTarget::None);
    }
    SECTION("Nada que insertar") {
        CHECK_FALSE(prepare_paste("", PasteTarget::MultiLine).has_value());
        CHECK_FALSE(prepare_paste("\x01\x02", PasteTarget::MultiLine).has_value());
        CHECK_FALSE(prepare_paste("\n", PasteTarget::SingleLine).has_value());
    }
    SECTION("Un salto de línea solo sí se pega con varias líneas") {
        CHECK(prepare_paste("\r\n", PasteTarget::MultiLine)->text == "\n");
    }
}

namespace {

/// Reloj falso: solo avanza cuando la prueba lo mueve.
struct FakeClock {
    std::chrono::steady_clock::time_point now{std::chrono::hours{1}};
    BracketedPaste::Clock clock() {
        return [this] { return now; };
    }
};

/// Pasa un texto como lo entrega FTXUI: un Character por carácter, "\n"
/// como Return y "\t" como Tab.
void feed_text(BracketedPaste& paste, std::string_view text) {
    std::size_t i = 0;
    while (i < text.size()) {
        if (text[i] == '\n') {
            CHECK(paste.feed(PasteKey::Return).consumed);
            ++i;
            continue;
        }
        if (text[i] == '\t') {
            CHECK(paste.feed(PasteKey::Tab).consumed);
            ++i;
            continue;
        }
        const std::size_t start = i;
        REQUIRE(chatbot::utf8::next_code_point(text, i).has_value());
        const auto step = paste.feed(PasteKey::Character, text.substr(start, i - start));
        CHECK(step.consumed);
        CHECK_FALSE(step.text.has_value());
    }
}

} // namespace

TEST_CASE("BracketedPaste acumula entre las marcas", "[paste]") {
    FakeClock time;
    BracketedPaste paste{time.clock()};
    SECTION("Fuera de un pegado no consume nada") {
        CHECK_FALSE(paste.active());
        CHECK_FALSE(paste.feed(PasteKey::Character, "a").consumed);
        CHECK_FALSE(paste.feed(PasteKey::Return).consumed);
        CHECK_FALSE(paste.feed(PasteKey::Tab).consumed);
        CHECK_FALSE(paste.feed(PasteKey::Other).consumed);
    }
    SECTION("Texto con Return y Tab en medio") {
        const auto start = paste.feed(PasteKey::Start);
        CHECK(start.consumed);
        CHECK_FALSE(start.text.has_value());
        CHECK(paste.active());
        feed_text(paste, "int main() {\n\treturn 0; // ñ\n}");
        const auto end = paste.feed(PasteKey::End);
        CHECK(end.consumed);
        REQUIRE(end.text.has_value());
        CHECK(*end.text == "int main() {\n\treturn 0; // ñ\n}");
        CHECK_FALSE(end.truncated);
        CHECK_FALSE(paste.active());
        // Después vuelve a dejar pasar las teclas.
        CHECK_FALSE(paste.feed(PasteKey::Return).consumed);
    }
    SECTION("Los eventos especiales se descartan") {
        (void)paste.feed(PasteKey::Start);
        feed_text(paste, "a");
        CHECK(paste.feed(PasteKey::Other).consumed);
        feed_text(paste, "b");
        CHECK(*paste.feed(PasteKey::End).text == "ab");
    }
    SECTION("El ratón y Event::Custom no rompen el acumulador") {
        (void)paste.feed(PasteKey::Start);
        feed_text(paste, "uno");
        const auto mouse = paste.feed(PasteKey::Passthrough);
        CHECK_FALSE(mouse.consumed);
        CHECK_FALSE(mouse.text.has_value());
        CHECK(paste.active());
        feed_text(paste, "\ndos");
        CHECK(*paste.feed(PasteKey::End).text == "uno\ndos");
    }
    SECTION("Un inicio sin fin seguido de otro inicio entrega el primero") {
        (void)paste.feed(PasteKey::Start);
        feed_text(paste, "primero");
        const auto again = paste.feed(PasteKey::Start);
        CHECK(again.consumed);
        REQUIRE(again.text.has_value());
        CHECK(*again.text == "primero");
        CHECK(paste.active());
        feed_text(paste, "segundo");
        CHECK(*paste.feed(PasteKey::End).text == "segundo");
    }
    SECTION("Un fin sin inicio se descarta") {
        const auto end = paste.feed(PasteKey::End);
        CHECK(end.consumed);
        CHECK_FALSE(end.text.has_value());
        CHECK_FALSE(paste.active());
    }
    SECTION("Un pegado vacío entrega texto vacío") {
        (void)paste.feed(PasteKey::Start);
        const auto end = paste.feed(PasteKey::End);
        REQUIRE(end.text.has_value());
        CHECK(end.text->empty());
    }
}

TEST_CASE("BracketedPaste deja de acumular en kMaxPasteBytes", "[paste]") {
    FakeClock time;
    BracketedPaste paste{time.clock()};
    (void)paste.feed(PasteKey::Start);
    bool all_consumed = true;
    for (std::size_t i = 0; i < kMaxPasteBytes; ++i) {
        all_consumed = paste.feed(PasteKey::Character, "a").consumed && all_consumed;
    }
    REQUIRE(all_consumed);
    // Lo que sigue se consume (no llega a la caja) pero no se guarda.
    CHECK(paste.feed(PasteKey::Character, "\xC3\xB1").consumed);
    CHECK(paste.feed(PasteKey::Return).consumed);
    CHECK(paste.feed(PasteKey::Character, "b").consumed);
    const auto end = paste.feed(PasteKey::End);
    REQUIRE(end.text.has_value());
    CHECK(end.text->size() == kMaxPasteBytes);
    CHECK(end.text->find('b') == std::string::npos);
    CHECK(end.truncated);

    // El siguiente pegado empieza limpio.
    (void)paste.feed(PasteKey::Start);
    (void)paste.feed(PasteKey::Character, "x");
    const auto next = paste.feed(PasteKey::End);
    CHECK(*next.text == "x");
    CHECK_FALSE(next.truncated);
}

TEST_CASE("BracketedPaste: salida de emergencia tras una pausa sin 201~", "[paste]") {
    using namespace std::chrono_literals;
    FakeClock time;
    BracketedPaste paste{time.clock()};

    SECTION("Un pegado normal, sin pausas, no se cierra antes de tiempo") {
        (void)paste.feed(PasteKey::Start);
        for (const char* piece : {"a", "b", "c"}) {
            time.now += 400ms; // Lento, pero sin pasar del límite.
            const auto step = paste.feed(PasteKey::Character, piece);
            CHECK(step.consumed);
            CHECK_FALSE(step.text.has_value());
        }
        time.now += 500ms; // Justo el límite todavía no cierra.
        CHECK(paste.feed(PasteKey::Return).consumed);
        const auto end = paste.feed(PasteKey::End);
        REQUIRE(end.text.has_value());
        CHECK(*end.text == "abc\n");
    }
    SECTION("Pausa de 600 ms y Ctrl+C: entrega lo pegado y Ctrl+C sigue (la app sale)") {
        (void)paste.feed(PasteKey::Start);
        feed_text(paste, "hola\nmundo");
        time.now += 600ms;
        // main.cpp traduce Ctrl+C (Event::CtrlC) como Other.
        const auto step = paste.feed(PasteKey::Other);
        CHECK_FALSE(step.consumed);
        REQUIRE(step.text.has_value());
        CHECK(*step.text == "hola\nmundo");
        CHECK_FALSE(paste.active());
    }
    SECTION("Pausa y un carácter: va a la caja, no al pegado") {
        (void)paste.feed(PasteKey::Start);
        feed_text(paste, "pegado");
        time.now += 600ms;
        const auto step = paste.feed(PasteKey::Character, "x");
        CHECK_FALSE(step.consumed);
        REQUIRE(step.text.has_value());
        CHECK(*step.text == "pegado");
        CHECK_FALSE(paste.active());
        // Las siguientes teclas tampoco son del pegado.
        CHECK_FALSE(paste.feed(PasteKey::Character, "y").consumed);
        // Y un 201~ que llegue tarde se descarta.
        const auto late = paste.feed(PasteKey::End);
        CHECK(late.consumed);
        CHECK_FALSE(late.text.has_value());
    }
    SECTION("Pausa y otro inicio: entrega el anterior y empieza otro") {
        (void)paste.feed(PasteKey::Start);
        feed_text(paste, "uno");
        time.now += 2s;
        const auto step = paste.feed(PasteKey::Start);
        CHECK(step.consumed);
        CHECK(*step.text == "uno");
        CHECK(paste.active());
        feed_text(paste, "dos");
        CHECK(*paste.feed(PasteKey::End).text == "dos");
    }
    SECTION("La pausa se mide desde el último evento del pegado, no desde el inicio") {
        (void)paste.feed(PasteKey::Start);
        for (int i = 0; i < 5; ++i) {
            time.now += 300ms;
            CHECK(paste.feed(PasteKey::Character, "a").consumed);
        }
        CHECK(*paste.feed(PasteKey::End).text == "aaaaa");
    }
    SECTION("El ratón tras la pausa también cierra, y no se consume") {
        (void)paste.feed(PasteKey::Start);
        feed_text(paste, "z");
        time.now += 600ms;
        const auto step = paste.feed(PasteKey::Passthrough);
        CHECK_FALSE(step.consumed);
        CHECK(*step.text == "z");
        CHECK_FALSE(paste.active());
    }
    SECTION("Un pegado recortado avisa también al cerrarse por la pausa") {
        (void)paste.feed(PasteKey::Start);
        (void)paste.feed(PasteKey::Character, std::string(kMaxPasteBytes, 'a'));
        (void)paste.feed(PasteKey::Character, "b");
        time.now += 600ms;
        const auto step = paste.feed(PasteKey::Other);
        CHECK(step.truncated);
        CHECK(step.text->size() == kMaxPasteBytes);
    }
}
