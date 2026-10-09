#include "chatbot/history.h"
#include "chatbot/types.h"
#include "chatbot/web_search.h"

#include "performance.hpp"
#include "system_prompt_invariants.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace {

using chatbot::Message;
using chatbot::Role;
using chatbot::TrimResult;
using chatbot::trim_history;

/// Texto de n bytes.
std::string bytes(std::size_t n) { return std::string(n, 'x'); }

/// Sistema (10 B) + tres vueltas completas (cada mensaje 100 B) + usuario actual (50 B).
std::vector<Message> long_conversation() {
    return {
        Message{Role::System, bytes(10)},
        Message{Role::User, "u1" + bytes(98)},     Message{Role::Assistant, "a1" + bytes(98)},
        Message{Role::User, "u2" + bytes(98)},     Message{Role::Assistant, "a2" + bytes(98)},
        Message{Role::User, "u3" + bytes(98)},     Message{Role::Assistant, "a3" + bytes(98)},
        Message{Role::User, "actual" + bytes(44)},
    };
}

std::size_t total_bytes(const std::vector<Message>& messages) {
    std::size_t total = 0;
    for (const Message& message : messages) {
        total += message.content.size();
    }
    return total;
}

} // namespace

TEST_CASE("trim_history: límite 0 no recorta nada", "[historial]") {
    const std::vector<Message> messages = long_conversation();
    const TrimResult result = trim_history(messages, 0);
    CHECK(result.messages.size() == messages.size());
    CHECK(result.dropped == 0);
}

TEST_CASE("trim_history: si todo cabe no recorta", "[historial]") {
    const std::vector<Message> messages = long_conversation(); // 660 B
    const TrimResult result = trim_history(messages, 660);
    CHECK(result.messages.size() == messages.size());
    CHECK(result.dropped == 0);
}

TEST_CASE("trim_history: recorta un par", "[historial]") {
    const TrimResult result = trim_history(long_conversation(), 600); // 660 - 200 = 460
    REQUIRE(result.messages.size() == 6);
    CHECK(result.dropped == 2);
    CHECK(result.messages[0].role == Role::System);
    CHECK(result.messages[1].content.substr(0, 2) == "u2");
    CHECK(total_bytes(result.messages) <= 600);
}

TEST_CASE("trim_history: recorta varios pares", "[historial]") {
    const TrimResult result = trim_history(long_conversation(), 300); // 660 - 400 = 260
    REQUIRE(result.messages.size() == 4);
    CHECK(result.dropped == 4);
    CHECK(result.messages[1].content.substr(0, 2) == "u3");
    CHECK(result.messages[2].content.substr(0, 2) == "a3");
    CHECK(result.messages[3].content.substr(0, 6) == "actual");
}

TEST_CASE("trim_history: sistema + último se conservan aunque excedan", "[historial]") {
    const TrimResult result = trim_history(long_conversation(), 5);
    REQUIRE(result.messages.size() == 2);
    CHECK(result.dropped == 6);
    CHECK(result.messages[0].role == Role::System);
    CHECK(result.messages[1].content.substr(0, 6) == "actual");
    CHECK(total_bytes(result.messages) > 5);
}

TEST_CASE("trim_history: los mensajes de sistema iniciales nunca se tocan", "[historial]") {
    std::vector<Message> messages = long_conversation();
    messages.insert(messages.begin() + 1, Message{Role::System, "segundo sistema"});
    const TrimResult result = trim_history(messages, 1);
    REQUIRE(result.messages.size() == 3);
    CHECK(result.messages[0].role == Role::System);
    CHECK(result.messages[0].content == bytes(10));
    CHECK(result.messages[1].role == Role::System);
    CHECK(result.messages[1].content == "segundo sistema");
    CHECK(result.messages[2].content.substr(0, 6) == "actual");
}

TEST_CASE("trim_history: tras recortar, lo primero después del sistema es User",
          "[historial]") {
    // Historial irregular: dos asistentes seguidos tras el primer usuario.
    const std::vector<Message> messages{
        Message{Role::System, "s"},
        Message{Role::User, bytes(100)},
        Message{Role::Assistant, bytes(100)},
        Message{Role::Assistant, bytes(100)},
        Message{Role::User, bytes(10)},
        Message{Role::Assistant, bytes(10)},
        Message{Role::User, bytes(10)},
    };
    for (std::size_t limit = 1; limit <= 400; limit += 7) {
        const TrimResult result = trim_history(messages, limit);
        REQUIRE(result.messages.size() >= 2);
        CHECK(result.messages[0].role == Role::System);
        CHECK(result.messages[1].role == Role::User);
        CHECK(result.messages.size() + result.dropped == messages.size());
    }
}

TEST_CASE("trim_history: dropped coincide con los mensajes quitados", "[historial]") {
    const std::vector<Message> messages = long_conversation();
    for (const std::size_t limit : {std::size_t{0}, std::size_t{1}, std::size_t{300},
                                    std::size_t{460}, std::size_t{461}, std::size_t{10000}}) {
        const TrimResult result = trim_history(messages, limit);
        CHECK(result.messages.size() + result.dropped == messages.size());
        CHECK(result.dropped % 2 == 0);
    }
}

TEST_CASE("trim_history: historial vacío o solo un mensaje", "[historial]") {
    CHECK(trim_history({}, 10).messages.empty());
    const TrimResult single = trim_history({Message{Role::User, bytes(100)}}, 10);
    CHECK(single.messages.size() == 1);
    CHECK(single.dropped == 0);
}

TEST_CASE("trim_history: sin mensaje de sistema, lo primero sigue siendo User",
          "[historial][sistema]") {
    std::vector<Message> messages = long_conversation();
    messages.erase(messages.begin()); // Instrucciones vacías: no hay System.
    const TrimResult result = trim_history(messages, 260);
    REQUIRE(result.dropped > 0);
    REQUIRE_FALSE(result.messages.empty());
    CHECK(result.messages.front().role == Role::User);
    CHECK(result.messages.back().content == messages.back().content);
    CHECK(total_bytes(result.messages) <= 260);
}

// Prueba de propiedades: historiales al azar (semilla fija) y los invariantes
// del mensaje de sistema en lo que queda después del recorte.

namespace {

using chatbot_test::check_invariants;
using chatbot_test::content_bytes;

bool is_continuation(char byte) {
    return (static_cast<unsigned char>(byte) & 0xC0U) == 0x80U;
}

/// Texto UTF-8 con caracteres de 1 a 4 bytes, del que se cortan los content.
const std::string& utf8_pool() {
    static const std::string pool = [] {
        const char* const pieces[] = {"a", "b", " ", "\n", "ñ", "é", "€", "日", "😀", "𝄞"};
        std::mt19937 generator{12345};
        std::string text;
        while (text.size() < 20000) {
            text += pieces[generator() % std::size(pieces)];
        }
        return text;
    }();
    return pool;
}

std::size_t uniform(std::mt19937& generator, std::size_t low, std::size_t high) {
    return std::uniform_int_distribution<std::size_t>{low, high}(generator);
}

/// Unos size bytes de texto UTF-8 válido (sin partir caracteres; puede
/// quedar hasta 3 bytes más corto). size <= 9000.
std::string utf8_text(std::mt19937& generator, std::size_t size) {
    const std::string& pool = utf8_pool();
    std::size_t begin = uniform(generator, 0, pool.size() - size - 4); // 4: avanzar al inicio de un carácter.
    while (is_continuation(pool[begin])) {
        ++begin;
    }
    std::size_t end = begin + size;
    while (end > begin && is_continuation(pool[end])) {
        --end;
    }
    return pool.substr(begin, end - begin);
}

/// Tamaño de un mensaje: casi siempre chico, a veces mediano, rara vez grande.
std::size_t message_size(std::mt19937& generator) {
    const std::size_t roll = uniform(generator, 0, 99);
    if (roll < 70) {
        return uniform(generator, 0, 200);
    }
    if (roll < 95) {
        return uniform(generator, 200, 2000);
    }
    return uniform(generator, 2000, 9000);
}

/// Un User del tamaño de un bloque de búsqueda: el bloque real de
/// format_search_context, con hasta kSearchTotalMaxBytes de content más su
/// encabezado.
std::string search_block(std::mt19937& generator) {
    chatbot::SearchResponse response;
    response.query = utf8_text(generator, uniform(generator, 1, 200));
    const std::size_t count = uniform(generator, 1, 6);
    for (std::size_t i = 0; i < count; ++i) {
        response.results.push_back(chatbot::SearchResult{
            utf8_text(generator, uniform(generator, 0, 80)), "https://ejemplo.com/" +
            std::to_string(i), utf8_text(generator, uniform(generator, 0, 1300)), "2026-10-09"});
    }
    return chatbot::format_search_context(response, "9 de octubre de 2026", "0123456789abcdef");
}

std::string random_user(std::mt19937& generator) {
    return uniform(generator, 0, 99) < 3 ? search_block(generator)
                                         : utf8_text(generator, message_size(generator));
}

struct TrimCase {
    std::string prompt; ///< "" = sin mensaje de sistema.
    std::vector<Message> messages;
    std::size_t limit = 0;
};

TrimCase random_case(std::mt19937& generator) {
    TrimCase test;
    if (uniform(generator, 0, 1) == 1) {
        test.prompt = utf8_text(generator, uniform(generator, 1, 9000));
        if (test.prompt.empty()) {
            test.prompt = "s"; // Un System nunca tiene content vacío.
        }
        test.messages.push_back(Message{Role::System, test.prompt});
    }
    const std::size_t pairs = uniform(generator, 1, 200);
    for (std::size_t i = 0; i < pairs; ++i) {
        test.messages.push_back(Message{Role::User, random_user(generator)});
        test.messages.push_back(
            Message{Role::Assistant, utf8_text(generator, message_size(generator))});
    }
    // El último User: a veces un bloque de búsqueda (un /buscar).
    test.messages.push_back(Message{Role::User, uniform(generator, 0, 4) == 0
                                                    ? search_block(generator)
                                                    : random_user(generator)});

    const std::size_t total = content_bytes(test.messages);
    switch (uniform(generator, 0, 5)) {
    case 0:
        test.limit = 0;
        break;
    case 1:
        test.limit = 1;
        break;
    case 2: // Menor que el sistema (o muy chico, si no hay sistema).
        test.limit = uniform(generator, 1, std::max<std::size_t>(test.prompt.size(), 2) - 1);
        break;
    case 3:
        test.limit = uniform(generator, 1, total);
        break;
    case 4: // Mayor o igual que todo.
        test.limit = total + uniform(generator, 0, 1000);
        break;
    default:
        test.limit = uniform(generator, 1, 40000);
        break;
    }
    return test;
}

/// Las propiedades del recorte, además de I1 a I4. "" si se cumplen.
std::string check_trim(const TrimCase& test, const TrimResult& result) {
    const std::vector<Message>& original = test.messages;
    if (std::string problem = check_invariants(result.messages, test.prompt,
                                               chatbot_test::Ending::Sent,
                                               original.back().content);
        !problem.empty()) {
        return problem;
    }
    if (result.messages.size() + result.dropped != original.size()) {
        return "dropped no coincide con los mensajes quitados";
    }
    // Lo conservado, sin el sistema, es un sufijo contiguo del original.
    const std::size_t system = test.prompt.empty() ? 0 : 1;
    const std::size_t kept = result.messages.size() - system;
    for (std::size_t i = 0; i < kept; ++i) {
        const Message& got = result.messages[system + i];
        const Message& expected = original[original.size() - kept + i];
        if (got.role != expected.role || got.content != expected.content) {
            return "lo conservado no es un sufijo contiguo del original (posición " +
                   std::to_string(system + i) + ")";
        }
    }
    const std::size_t total = content_bytes(result.messages);
    if (test.limit == 0 && result.dropped != 0) {
        return "con límite 0 se quitó algo";
    }
    // Si sigue excediendo es porque ya no hay nada más que quitar.
    if (test.limit > 0 && total > test.limit && result.messages.size() != system + 1) {
        return "excede el límite y aún quedaban pares por quitar";
    }
    // Se para al caber: con el último par quitado de vuelta ya no cabría.
    if (result.dropped >= 2) {
        const std::size_t first_kept = original.size() - kept;
        const std::size_t pair = original[first_kept - 1].content.size() +
                                 original[first_kept - 2].content.size();
        if (total + pair <= test.limit) {
            return "se quitó un par que sí cabía";
        }
    }
    return {};
}

} // namespace

TEST_CASE("trim_history: propiedades con historiales al azar", "[historial][sistema][propiedades]") {
    constexpr std::uint32_t kSeed = 20261009;
    constexpr int kCases = 2000;
    std::mt19937 generator{kSeed};
    int failures = 0;
    std::size_t trimmed_cases = 0;
    std::size_t exceeded_cases = 0;
    const auto start = std::chrono::steady_clock::now();
    for (int index = 0; index < kCases && failures < 5; ++index) {
        const TrimCase test = random_case(generator);
        const TrimResult result = trim_history(test.messages, test.limit);
        trimmed_cases += result.dropped > 0 ? 1 : 0;
        exceeded_cases +=
            test.limit > 0 && content_bytes(result.messages) > test.limit ? 1 : 0;
        if (const std::string problem = check_trim(test, result); !problem.empty()) {
            ++failures;
            FAIL_CHECK("semilla " << kSeed << ", caso " << index << ": " << problem);
        }
    }
    const double ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - start)
                          .count();
    WARN("trim_history: " << kCases << " historiales al azar en " << ms << " ms ("
                          << trimmed_cases << " recortados, " << exceeded_cases
                          << " siguen excediendo)");
    CHECK(failures == 0);
    // La muestra cubre los dos casos interesantes.
    CHECK(trimmed_cases > 100);
    CHECK(exceeded_cases > 100);
    if (chatbot_test::strict_performance()) {
        CHECK(ms < 2000.0);
    }
}

TEST_CASE("check_invariants: detecta cada violación", "[historial][sistema]") {
    using chatbot_test::Ending;
    const std::string prompt = "P";
    const std::vector<Message> good{Message{Role::System, "P"}, Message{Role::User, "u1"},
                                    Message{Role::Assistant, "a1"}, Message{Role::User, "u2"}};
    CHECK(check_invariants(good, prompt).empty());
    CHECK(check_invariants(good, prompt, Ending::Sent, "u2").empty());

    // I1: falta el sistema o no es idéntico byte por byte.
    CHECK_FALSE(check_invariants({good.begin() + 1, good.end()}, prompt).empty());
    CHECK_FALSE(check_invariants(good, "P ").empty());
    // I2: un segundo System, al inicio o en medio; o uno con prompt vacío.
    std::vector<Message> twice = good;
    twice.insert(twice.begin() + 1, Message{Role::System, "P"});
    CHECK_FALSE(check_invariants(twice, prompt).empty());
    std::vector<Message> middle = good;
    middle.insert(middle.begin() + 2, Message{Role::System, "P"});
    CHECK_FALSE(check_invariants(middle, prompt).empty());
    CHECK_FALSE(check_invariants(good, "").empty());
    // I3: no termina en User, o no es el que se acaba de enviar.
    CHECK_FALSE(check_invariants({good.begin(), good.end() - 1}, prompt).empty());
    CHECK_FALSE(check_invariants(good, prompt, Ending::Sent, "otro").empty());
    // I4: empieza con Assistant o rompe la alternancia.
    CHECK_FALSE(check_invariants({Message{Role::System, "P"}, Message{Role::Assistant, "a"},
                                  Message{Role::User, "u"}},
                                 prompt)
                    .empty());
    CHECK_FALSE(check_invariants({Message{Role::System, "P"}, Message{Role::User, "u1"},
                                  Message{Role::User, "u2"}},
                                 prompt)
                    .empty());
    // Sin petición en curso: no puede quedar un User huérfano al final.
    CHECK(check_invariants({good.begin(), good.end() - 1}, prompt, Ending::Idle).empty());
    CHECK_FALSE(check_invariants(good, prompt, Ending::Idle).empty());
    CHECK(check_invariants({Message{Role::System, "P"}}, prompt, Ending::Idle).empty());
    CHECK(check_invariants({}, "", Ending::Idle).empty());
}

// Caso límite documentado en chatbot/history.h: el sistema y el último
// mensaje se mandan aunque juntos excedan el límite.

namespace {

std::vector<Message> big_system_history(std::string last) {
    std::vector<Message> messages{Message{Role::System, bytes(8000)}};
    for (int i = 0; i < 5; ++i) {
        messages.push_back(Message{Role::User, "u" + std::to_string(i) + bytes(100)});
        messages.push_back(Message{Role::Assistant, "a" + std::to_string(i) + bytes(100)});
    }
    messages.push_back(Message{Role::User, std::move(last)});
    return messages;
}

} // namespace

TEST_CASE("trim_history: sistema de 8000 B con límite de 1000 B manda sistema y último",
          "[historial][sistema]") {
    const std::vector<Message> messages = big_system_history("pregunta actual");
    const TrimResult result = trim_history(messages, 1000);

    CHECK(check_invariants(result.messages, bytes(8000), chatbot_test::Ending::Sent,
                           "pregunta actual") == "");
    REQUIRE(result.messages.size() == 2);
    CHECK(result.dropped == 10);
    CHECK(content_bytes(result.messages) > 1000); // Excede, a propósito.
}

TEST_CASE("trim_history: sistema de 8000 B con límite de 1000 B y un bloque de búsqueda al final",
          "[historial][sistema][busqueda]") {
    chatbot::SearchResponse response;
    response.query = "consulta";
    for (int i = 0; i < 5; ++i) {
        response.results.push_back(chatbot::SearchResult{
            "Título " + std::to_string(i), "https://ejemplo.com/" + std::to_string(i),
            std::string(chatbot::kSearchContentMaxBytes, 'c'), ""});
    }
    const std::string block =
        chatbot::format_search_context(response, "9 de octubre de 2026", "0123456789abcdef");
    REQUIRE(block.size() > chatbot::kSearchTotalMaxBytes); // Más grande que el límite solo.

    const std::vector<Message> messages = big_system_history(block);
    const TrimResult result = trim_history(messages, 1000);

    CHECK(check_invariants(result.messages, bytes(8000), chatbot_test::Ending::Sent, block) ==
          "");
    REQUIRE(result.messages.size() == 2);
    CHECK(result.dropped == 10);
    CHECK(result.messages.back().content.find("Pregunta del usuario: consulta\n") !=
          std::string::npos);
}
