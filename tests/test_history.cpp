#include "chatbot/history.h"
#include "chatbot/types.h"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
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
