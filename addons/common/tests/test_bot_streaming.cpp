// Vista previa mientras llega la respuesta (Draft), mensajes que ya no caben
// (StreamingReply), "Detener" (Bot::stop), respaldo en texto plano y
// mensajes que llegaron tarde.

#include "bot_harness.hpp"
#include "performance.hpp"

#include "chatbot/bot/replies.h"
#include "chatbot/bot/split.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

using chatbot::Message;
using chatbot::bot::MessageLimit;
using chatbot::bot::StreamingReply;
using chatbot_bot_test::BotHarness;
using chatbot_bot_test::DraftEvent;
using chatbot_bot_test::Sent;
using chatbot_bot_test::TextFormat;
using chatbot_test::sent_messages_of;
using namespace std::chrono_literals;

/// Sin espacios: lo que debe conservarse al partir (los cortes pueden
/// quitar espacios).
std::string squeeze(std::string_view text) {
    std::string out;
    for (const char c : text) {
        if (c != ' ' && c != '\n' && c != '\t' && c != '\r') {
            out.push_back(c);
        }
    }
    return out;
}

MessageLimit bytes(std::size_t max) {
    MessageLimit limit;
    limit.max_size = max;
    return limit;
}

/// Avanza el reloj falso 300 ms cada 20 ms (para que haya vistas previas
/// mientras el modelo escribe de verdad). Se detiene y se une al destruirse,
/// también si un REQUIRE sale antes.
class ClockTicker {
public:
    explicit ClockTicker(chatbot_bot_test::FakeClock& clock)
        : thread_{[this, &clock] {
              while (ticking_) {
                  clock.advance(300ms);
                  std::this_thread::sleep_for(20ms);
              }
          }} {}
    ~ClockTicker() { stop(); }
    ClockTicker(const ClockTicker&) = delete;
    ClockTicker& operator=(const ClockTicker&) = delete;

    void stop() {
        ticking_ = false;
        if (thread_.joinable()) {
            thread_.join();
        }
    }

private:
    std::atomic<bool> ticking_{true};
    std::thread thread_;
};

/// Las vistas previas con texto (update) de un chat.
std::vector<DraftEvent> updates_of(const std::vector<DraftEvent>& events) {
    std::vector<DraftEvent> out;
    for (const DraftEvent& event : events) {
        if (!event.status) {
            out.push_back(event);
        }
    }
    return out;
}

} // namespace

TEST_CASE("StreamingReply: mientras cabe, todo queda pendiente", "[bot][streaming]") {
    StreamingReply reply{bytes(100)};
    reply.append("Hola, ");
    reply.append("mundo.");
    CHECK(reply.take_complete().empty());
    CHECK(reply.pending() == "Hola, mundo.");
    CHECK_FALSE(reply.has_taken());
    CHECK(reply.text() == "Hola, mundo.");
}

TEST_CASE("StreamingReply: corta entre bloques y el resto sigue pendiente", "[bot][streaming]") {
    StreamingReply reply{bytes(56)};
    reply.append("Primer párrafo corto.\n\nSegundo párrafo, que ");
    reply.append("ya no cabe con el primero.");
    const std::vector<std::string> taken = reply.take_complete();
    REQUIRE(taken.size() == 1);
    CHECK(taken[0] == "Primer párrafo corto.");
    CHECK(reply.pending() == "Segundo párrafo, que ya no cabe con el primero.");
    CHECK(reply.has_taken());
    // El texto completo no cambia: es el que entra al historial.
    CHECK(reply.text() ==
          "Primer párrafo corto.\n\nSegundo párrafo, que ya no cabe con el primero.");
}

TEST_CASE("StreamingReply: un corte dentro de código repite la cerca", "[bot][streaming]") {
    StreamingReply reply{bytes(48)};
    reply.append("```python\nlinea_uno = 1\nlinea_dos = 2\nlinea_tres = 3\n");
    const std::vector<std::string> taken = reply.take_complete();
    REQUIRE_FALSE(taken.empty());
    CHECK(taken[0].rfind("```python\n", 0) == 0);
    const std::string pending = reply.pending();
    CHECK(pending.rfind("```python\n", 0) == 0);
    reply.append("linea_cuatro = 4\n```\n\nDespués del código.");
    for (const std::string& piece : reply.take_complete()) {
        CHECK(chatbot::bot::fits_in_message(piece, bytes(48)));
    }
    CHECK(reply.pending().find("Después del código.") != std::string::npos);
}

TEST_CASE("StreamingReply: justo antes de la cerca que cierra, el resto no la repite",
          "[bot][streaming]") {
    // 31 bytes caben: "```\nabcdefghijklmnopqrstuvwxyz\n" es la cerca y el
    // contenido; el corte cae antes de la cerca que cierra.
    StreamingReply reply{bytes(32)};
    reply.append("```\nabcdefghijklmnopqrstuvwxyz\n```\nfin del texto\n");
    const std::vector<std::string> taken = reply.take_complete();
    const std::string pending = reply.pending();
    CHECK(pending.find("```") == std::string::npos);
    CHECK(pending.find("fin del texto") != std::string::npos);
    std::string joined;
    for (const std::string& piece : taken) {
        joined += piece;
    }
    CHECK(joined.find("abcdefghijklmnopqrstuvwxyz") != std::string::npos);
}

TEST_CASE("StreamingReply: una línea larga sin bloques se parte en espacios", "[bot][streaming]") {
    StreamingReply reply{bytes(20)};
    reply.append("uno dos tres cuatro cinco seis siete ocho nueve diez");
    std::string joined;
    for (const std::string& piece : reply.take_complete()) {
        CHECK(piece.size() <= 20);
        joined += piece + " ";
    }
    CHECK(squeeze(joined + reply.pending()) == squeeze("unodostrescuatrocincoseissieteochonuevediez"));
    CHECK(reply.pending().size() <= 20);
}

TEST_CASE("StreamingReply: por deltas al azar, nada se pierde y todo cabe", "[bot][streaming]") {
    const std::string answer =
        "# Título\n\nUn párrafo con **negritas** y `código`.\n\n"
        "- uno\n- dos\n- tres\n\n"
        "```cpp\nint main() {\n    return 0;\n}\n```\n\n"
        "| a | b |\n|---|---|\n| 1 | 2 |\n| 3 | 4 |\n\n"
        "Otro párrafo bastante más largo, con varias palabras para que haya que partirlo "
        "en espacios cuando no quepa en un solo mensaje del tope chico.\n\n"
        "> una cita\n> de dos líneas\n\nFin.";
    std::mt19937 random{1234};
    for (const std::size_t max : {std::size_t{24}, std::size_t{40}, std::size_t{80}}) {
        for (int round = 0; round < 20; ++round) {
            StreamingReply reply{bytes(max)};
            std::vector<std::string> pieces;
            std::size_t pos = 0;
            while (pos < answer.size()) {
                const std::size_t step = 1 + random() % 9;
                reply.append(std::string_view{answer}.substr(pos, step));
                pos += step;
                for (std::string& piece : reply.take_complete()) {
                    pieces.push_back(std::move(piece));
                }
                CHECK(chatbot::bot::fits_in_message(reply.pending(), bytes(max)));
            }
            std::string joined;
            for (const std::string& piece : pieces) {
                CHECK_FALSE(piece.empty());
                CHECK(chatbot::bot::fits_in_message(piece, bytes(max)));
                joined += piece + "\n";
            }
            joined += reply.pending();
            // Sin espacios y sin las cercas que se repiten, es lo mismo.
            std::string expected = squeeze(answer);
            std::string got = squeeze(joined);
            for (std::string* text : {&expected, &got}) {
                for (std::size_t at = text->find("```cpp"); at != std::string::npos;
                     at = text->find("```cpp")) {
                    text->erase(at, 6);
                }
                for (std::size_t at = text->find("```"); at != std::string::npos;
                     at = text->find("```")) {
                    text->erase(at, 3);
                }
            }
            CHECK(got == expected);
            CHECK(reply.text() == answer);
        }
    }
}

TEST_CASE("bot: la vista previa se actualiza como mucho cada draft_interval", "[bot][streaming]") {
    BotHarness h;
    h.outbox.drafts_enabled = true;
    h.outbox.clock = &h.clock;
    std::vector<std::string> deltas;
    for (int i = 0; i < 30; ++i) {
        deltas.push_back("palabra" + std::to_string(i) + " ");
    }
    chatbot::bot::BotConfig config = chatbot_bot_test::test_bot_config();
    config.draft_interval = 1000ms;
    // Cada trozo llega 300 ms después del anterior.
    h.start(config, chatbot_bot_test::stepped_clients(deltas, [&h] { h.clock.advance(300ms); }));
    h.say(10, 1, "hola");

    const std::vector<DraftEvent> events = h.outbox.drafts();
    REQUIRE_FALSE(events.empty());
    CHECK(events.front().status);
    CHECK(events.front().text == chatbot::bot::kThinkingStatus);
    const std::vector<DraftEvent> updates = updates_of(events);
    REQUIRE(updates.size() >= 3);
    auto previous = events.front().at;
    for (const DraftEvent& update : updates) {
        CHECK(update.at - previous >= 1000ms);
        previous = update.at;
        CHECK(update.reply == events.front().reply);
    }
    // Cada vista previa es un prefijo de la respuesta, que sale completa al
    // final como un solo mensaje.
    std::string answer;
    for (const std::string& delta : deltas) {
        answer += delta;
    }
    while (!answer.empty() && answer.back() == ' ') {
        answer.pop_back();
    }
    for (const DraftEvent& update : updates) {
        CHECK(answer.rfind(update.text, 0) == 0);
    }
    CHECK(h.outbox.texts_for(10) == std::vector<std::string>{answer});
    CHECK(h.outbox.closed_drafts == 1);
    // Con vista previa no hace falta "escribiendo…".
    CHECK(h.outbox.typing().empty());
}

TEST_CASE("bot: una vista previa rechazada no tumba la respuesta", "[bot][streaming]") {
    BotHarness h;
    h.outbox.drafts_enabled = true;
    h.outbox.reject_drafts = true;
    chatbot::bot::BotConfig config = chatbot_bot_test::test_bot_config();
    config.draft_interval = 250ms;
    h.start(config, chatbot_bot_test::stepped_clients({"```py\nprint(1)\n", "print(2)\n", "```\n"},
                                                      [&h] { h.clock.advance(500ms); }));
    h.say(10, 1, "hola");
    CHECK_FALSE(updates_of(h.outbox.drafts()).empty());
    CHECK(h.outbox.texts_for(10) == std::vector<std::string>{"```py\nprint(1)\nprint(2)\n```"});
    CHECK(h.log.contains("vistas_rechazadas="));
    CHECK_FALSE(h.log.contains("vistas_rechazadas=0"));
}

TEST_CASE("bot: lo que ya no cabe sale como mensaje final mientras llega el resto",
          "[bot][streaming]") {
    BotHarness h;
    h.outbox.drafts_enabled = true;
    h.outbox.markdown_limit = bytes(60);
    std::vector<std::string> deltas;
    std::string answer;
    for (int i = 0; i < 12; ++i) {
        const std::string paragraph = "Párrafo número " + std::to_string(i) + " de la respuesta.\n\n";
        deltas.push_back(paragraph);
        answer += paragraph;
    }
    chatbot::bot::BotConfig config = chatbot_bot_test::test_bot_config();
    config.draft_interval = 250ms;
    h.start(config, chatbot_bot_test::stepped_clients(deltas, [&h] { h.clock.advance(250ms); }));
    h.say(10, 1, "hola");

    const std::vector<Sent> sent = h.outbox.sent();
    REQUIRE(sent.size() > 2);
    std::string joined;
    for (const Sent& piece : sent) {
        CHECK(piece.format == TextFormat::Markdown);
        CHECK(chatbot::bot::fits_in_message(piece.text, bytes(60)));
        joined += piece.text + "\n";
    }
    CHECK(squeeze(joined) == squeeze(answer));
    // Mensajes finales en medio de la respuesta, no todos al final: entre
    // ellos hubo vistas previas.
    for (const DraftEvent& update : updates_of(h.outbox.drafts())) {
        CHECK(chatbot::bot::fits_in_message(update.text, bytes(60)));
    }
}

TEST_CASE("bot: sin vista previa, la respuesta sale completa al final", "[bot][streaming]") {
    BotHarness h;
    h.outbox.markdown_limit = bytes(60);
    h.start(chatbot_bot_test::test_bot_config(),
            chatbot_bot_test::stepped_clients({"uno ", "dos ", "tres"}, [&h] { h.clock.advance(2s); }));
    h.say(10, 1, "hola");
    CHECK(h.outbox.drafts().empty());
    CHECK(h.outbox.texts_for(10) == std::vector<std::string>{"uno dos tres"});
    // "Escribiendo…" mientras, como antes.
    CHECK(h.outbox.typing() ==
          std::vector<std::pair<chatbot::bot::ChatId, bool>>{{10, true}, {10, false}});
}

TEST_CASE("bot: Detener cancela en menos de 2 s y manda lo generado con la nota",
          "[bot][streaming]") {
    BotHarness h;
    h.outbox.drafts_enabled = true;
    chatbot::bot::BotConfig config = chatbot_bot_test::test_bot_config();
    config.draft_interval = 250ms;
    // Un trozo cada 50 ms durante mucho tiempo; el reloj falso avanza con
    // cada uno para que haya vistas previas.
    h.start(config, [] {
        return std::make_unique<chatbot::ChatClient>(
            chatbot_test::test_config(),
            std::make_unique<chatbot_bot_test::SlowTransport>(100000, 50ms),
            std::make_unique<chatbot_test::FakeSleeper>());
    });
    ClockTicker ticker{h.clock};
    h.bot->handle(chatbot_bot_test::message(10, 1, "hola"));
    const chatbot::bot::ReplyId reply = h.outbox.wait_for_update();
    REQUIRE(reply != 0);
    CHECK_FALSE(h.bot->stop(10, reply + 1)); // Otra respuesta: no hace nada.
    CHECK_FALSE(h.bot->stop(11, reply));
    // Se anota (como info: es normal si la respuesta acaba de terminar).
    CHECK(h.log.level_of("detener_sin_respuesta chat=10") == chatbot::bot::LogLevel::Info);
    CHECK(h.log.level_of("detener_sin_respuesta chat=11") == chatbot::bot::LogLevel::Info);
    CHECK_FALSE(h.log.contains("detener chat="));
    const auto started = std::chrono::steady_clock::now();
    CHECK(h.bot->stop(10, reply));
    REQUIRE(h.bot->wait_idle(chatbot_test::kThreadTimeout));
    chatbot_test::report_latency("Detener a la mitad", std::chrono::steady_clock::now() - started);
    ticker.stop();

    const std::vector<std::string> texts = h.outbox.texts_for(10);
    REQUIRE(texts.size() == 2);
    // Lo que se alcanzó a generar (al menos lo que se vio en la vista previa).
    CHECK(texts[0].rfind("eco:", 0) == 0);
    CHECK(texts[1] == chatbot::bot::kStoppedNote);
    CHECK(h.log.contains("detenida chat=10 usuario=1"));
    CHECK_FALSE(h.bot->stop(10, reply)); // Ya terminó.
}

TEST_CASE("bot: lo detenido no entra al historial", "[bot][streaming]") {
    BotHarness h;
    h.outbox.drafts_enabled = true;
    chatbot::bot::BotConfig config = chatbot_bot_test::test_bot_config();
    config.draft_interval = 250ms;
    std::atomic<bool> slow{true};
    auto recorder = h.recorder;
    h.start(config, [&slow, recorder] {
        if (slow) {
            return std::make_unique<chatbot::ChatClient>(
                chatbot_test::test_config(),
                std::make_unique<chatbot_bot_test::SlowTransport>(100000, 20ms),
                std::make_unique<chatbot_test::FakeSleeper>());
        }
        return chatbot_bot_test::scripted_clients(recorder)();
    });
    ClockTicker ticker{h.clock};
    h.bot->handle(chatbot_bot_test::message(10, 1, "primera"));
    const chatbot::bot::ReplyId reply = h.outbox.wait_for_update();
    REQUIRE(h.bot->stop(10, reply));
    REQUIRE(h.bot->wait_idle(chatbot_test::kThreadTimeout));
    ticker.stop();

    slow = false;
    h.say(10, 1, "segunda");
    // Solo el sistema y la segunda pregunta: la detenida no entró.
    const std::vector<Message> sent = sent_messages_of(h.recorder->requests().back());
    REQUIRE(sent.size() == 2);
    CHECK(sent[1].content == "segunda");
}

TEST_CASE("bot: Detener antes del primer texto manda solo el aviso", "[bot][streaming]") {
    BotHarness h;
    h.outbox.drafts_enabled = true;
    auto gate = std::make_shared<chatbot_bot_test::Gate>();
    h.start(chatbot_bot_test::test_bot_config(), chatbot_bot_test::gated_clients(gate, h.recorder));
    h.bot->handle(chatbot_bot_test::message(10, 1, "hola"));
    REQUIRE(gate->wait_inside(1));
    const std::vector<DraftEvent> events = h.outbox.drafts();
    REQUIRE(events.size() == 1);
    REQUIRE(h.bot->stop(10, events.front().reply));
    REQUIRE(h.bot->wait_idle(chatbot_test::kThreadTimeout));
    CHECK(h.outbox.texts_for(10) == std::vector<std::string>{std::string{chatbot::bot::kStoppedEmpty}});
}

namespace {

/// Los ReplyId de las vistas previas, en el orden en que aparecieron.
std::vector<chatbot::bot::ReplyId> reply_ids(const std::vector<DraftEvent>& events) {
    std::vector<chatbot::bot::ReplyId> ids;
    for (const DraftEvent& event : events) {
        if (ids.empty() || ids.back() != event.reply) {
            ids.push_back(event.reply);
        }
    }
    return ids;
}

/// El ReplyId de la primera respuesta de un Bot nuevo con esta fuente.
chatbot::bot::ReplyId first_reply_of(chatbot::bot::ReplyIdSource source) {
    BotHarness h;
    h.outbox.drafts_enabled = true;
    h.first_reply = std::move(source);
    h.start();
    h.say(10, 1, "hola");
    const std::vector<chatbot::bot::ReplyId> ids = reply_ids(h.outbox.drafts());
    REQUIRE(ids.size() == 1);
    return ids.front();
}

/// Cuántos valores distintos dan los primeros ReplyId de `count` arranques.
std::size_t distinct_first_replies(const chatbot::bot::ReplyIdSource& source, int count) {
    std::vector<chatbot::bot::ReplyId> firsts;
    for (int i = 0; i < count; ++i) {
        firsts.push_back(first_reply_of(source));
        CHECK(firsts.back() >= 1);
        CHECK(firsts.back() <= chatbot::bot::kMaxReplyId);
    }
    std::sort(firsts.begin(), firsts.end());
    return static_cast<std::size_t>(std::unique(firsts.begin(), firsts.end()) - firsts.begin());
}

} // namespace

TEST_CASE("bot: un reinicio no repite el primer ReplyId", "[bot][streaming][detener]") {
    // Telegram ignora en silencio un draft_id que ya se detuvo en ese chat:
    // si cada arranque empezara en 1, la primera respuesta saldría sin
    // vista previa ni Detener.
    // Con 2^31 valores posibles, que 5 arranques den el mismo valor tiene
    // probabilidad 2^-124: solo pasa con un sorteo roto. Si cada arranque
    // empezara en 1, darían 1 siempre.
    CHECK(distinct_first_replies(chatbot::bot::random_reply_id, 5) >= 2);
    // Sin first_reply, la real.
    CHECK(distinct_first_replies({}, 5) >= 2);
}

TEST_CASE("bot: random_reply_id da valores del rango y distintos", "[bot][streaming]") {
    std::vector<chatbot::bot::ReplyId> values;
    for (int i = 0; i < 1000; ++i) {
        values.push_back(chatbot::bot::random_reply_id());
        CHECK(values.back() >= 1);
        CHECK(values.back() <= chatbot::bot::kMaxReplyId);
    }
    std::sort(values.begin(), values.end());
    // Con 2^31 valores posibles, la probabilidad de algún par repetido en
    // 1000 es ~2.3e-4: se piden 990 distintos, no 1000, para que la prueba
    // no falle sola; un sorteo roto (el reloj, un valor fijo) da muchos menos.
    CHECK(std::unique(values.begin(), values.end()) - values.begin() >= 990);
}

TEST_CASE("bot: los ReplyId vuelven a 1 después del tope y nunca dan 0",
          "[bot][streaming][detener]") {
    constexpr chatbot::bot::ReplyId kMax = chatbot::bot::kMaxReplyId;
    BotHarness h;
    h.outbox.drafts_enabled = true;
    h.first_reply = [] { return kMax - 1; };
    h.start();
    for (int i = 0; i < 4; ++i) {
        h.say(10, 1, "hola " + std::to_string(i));
    }
    CHECK(reply_ids(h.outbox.drafts()) ==
          std::vector<chatbot::bot::ReplyId>{kMax - 1, kMax, 1, 2});

    // Una fuente fuera del rango se lleva a él.
    CHECK(first_reply_of([] { return kMax; }) == kMax);
    CHECK(first_reply_of([] { return chatbot::bot::ReplyId{0}; }) == 1);
    CHECK(first_reply_of([] { return kMax + 1; }) == 2);
    for (const chatbot::bot::ReplyId value :
         {chatbot::bot::ReplyId{-1}, chatbot::bot::ReplyId{-5}, kMax * 3,
          std::numeric_limits<chatbot::bot::ReplyId>::min(),
          std::numeric_limits<chatbot::bot::ReplyId>::max()}) {
        const chatbot::bot::ReplyId reply = first_reply_of([value] { return value; });
        CHECK(reply >= 1);
        CHECK(reply <= kMax);
    }
    // Una fuente que lanza: la real, sin tumbar al bot.
    const chatbot::bot::ReplyId fallback =
        first_reply_of([]() -> chatbot::bot::ReplyId { throw std::runtime_error{"sin fuente"}; });
    CHECK(fallback >= 1);
    CHECK(fallback <= kMax);
}

TEST_CASE("bot: Detener funciona con el ReplyId del tope y con el que le sigue",
          "[bot][streaming][detener]") {
    BotHarness h;
    h.outbox.drafts_enabled = true;
    h.first_reply = [] { return chatbot::bot::kMaxReplyId; };
    auto gate = std::make_shared<chatbot_bot_test::Gate>();
    h.start(chatbot_bot_test::test_bot_config(), chatbot_bot_test::gated_clients(gate, h.recorder));
    for (const chatbot::bot::ReplyId expected : {chatbot::bot::kMaxReplyId, chatbot::bot::ReplyId{1}}) {
        const std::size_t seen = h.outbox.drafts().size();
        h.bot->handle(chatbot_bot_test::message(10, 1, "hola"));
        REQUIRE(gate->wait_inside(1));
        const std::vector<DraftEvent> events = h.outbox.drafts();
        REQUIRE(events.size() == seen + 1);
        CHECK(events.back().reply == expected);
        CHECK_FALSE(h.bot->stop(10, expected == 1 ? 2 : expected - 1));
        REQUIRE(h.bot->stop(10, events.back().reply));
        REQUIRE(h.bot->wait_idle(chatbot_test::kThreadTimeout));
    }
    CHECK(h.outbox.texts_for(10) == std::vector<std::string>{std::string{chatbot::bot::kStoppedEmpty},
                                                              std::string{chatbot::bot::kStoppedEmpty}});
}

TEST_CASE("bot: un Markdown rechazado se manda como texto plano partido a su tope",
          "[bot][streaming]") {
    BotHarness h{50}; // Texto plano: 50.
    h.outbox.markdown_limit = bytes(400);
    h.outbox.reject_markdown = true;
    std::string answer;
    for (int i = 0; i < 12; ++i) {
        answer += "palabra" + std::to_string(i) + " ";
    }
    h.start(chatbot_bot_test::test_bot_config(),
            chatbot_bot_test::scripted_clients(h.recorder, [&answer](const std::vector<Message>&) {
                return chatbot_bot_test::ok_stream({answer});
            }));
    h.say(10, 1, "hola");
    const std::vector<Sent> sent = h.outbox.sent();
    REQUIRE(sent.size() > 1);
    std::string joined;
    for (const Sent& piece : sent) {
        CHECK(piece.format == TextFormat::Plain);
        CHECK(chatbot::bot::utf16_length(piece.text) <= 50);
        joined += piece.text + " ";
    }
    CHECK(squeeze(joined) == squeeze(answer));
    CHECK(h.log.contains("enviado_como_texto chat=10 pedazo=1/1"));
}

TEST_CASE("bot: un mensaje que llegó tarde no va al modelo ni cuenta", "[bot][viejos]") {
    BotHarness h;
    chatbot::bot::BotConfig config = chatbot_bot_test::test_bot_config();
    config.daily_message_limit = 1;
    config.stale_after = std::chrono::minutes{10};
    h.start(config);

    chatbot::bot::IncomingMessage old = chatbot_bot_test::message(10, 1, "¿sigues ahí?");
    old.sent_at = h.clock.now() - std::chrono::minutes{11};
    h.bot->handle(old);
    chatbot::bot::IncomingMessage search = chatbot_bot_test::message(10, 1, "/buscar algo");
    search.sent_at = old.sent_at;
    h.bot->handle(search); // Sin búsqueda configurada: el aviso de siempre.
    chatbot::bot::IncomingMessage help = chatbot_bot_test::message(10, 1, "/ayuda");
    help.sent_at = old.sent_at;
    h.bot->handle(help); // Los comandos sí se atienden.
    REQUIRE(h.bot->wait_idle(chatbot_test::kThreadTimeout));
    const std::vector<std::string> texts = h.outbox.texts_for(10);
    REQUIRE(texts.size() == 3);
    CHECK(texts[0] == chatbot::bot::kStaleMessage);
    CHECK(texts[1] == chatbot::bot::kSearchUnavailable);
    CHECK(texts[2].find("/nuevo") != std::string::npos);
    CHECK(h.recorder->count() == 0);
    CHECK(h.log.contains("mensaje_viejo chat=10 usuario=1"));

    // Uno reciente sí, y no se gastó el tope diario.
    chatbot::bot::IncomingMessage recent = chatbot_bot_test::message(10, 1, "hola");
    recent.sent_at = h.clock.now() - std::chrono::minutes{9};
    h.bot->handle(recent);
    REQUIRE(h.bot->wait_idle(chatbot_test::kThreadTimeout));
    CHECK(h.outbox.texts_for(10).back() == "eco: hola");
}
