// Forma del token de @BotFather y códigos de invitación generados.

#include "chatbot/addon_settings/limits.h"
#include "chatbot/addon_settings/token.h"

#include "chatbot/platform.h"

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string>

namespace settings = chatbot::addon_settings;

TEST_CASE("token: tiene que tener la forma de uno de @BotFather", "[addon_settings][token]") {
    CHECK(settings::plausible_token("123456789:AAHdqTcvCH1vGWJxfSeofSAs0K5PALDsaw"));
    for (const char* bad : {"", "123456", ":AAAAAAAAAAAAAAAAAAAAAAAA", "abc:AAAAAAAAAAAAAAAAAAAAAAAA",
                            "123:corto", "123:AAAAAAAAAAAAAAAAAAAAAAAA/getMe?x=",
                            "123:AAAAAAAAAAAAAAAAAAAA AAAA", "123:AAAAAAAAAAAAAAAAAAAAAAA#"}) {
        INFO(bad);
        CHECK_FALSE(settings::plausible_token(bad));
    }
}

TEST_CASE("código de invitación: 16 caracteres válidos y distintos cada vez",
          "[addon_settings][token]") {
    if (chatbot::current_os() == chatbot::Os::Windows) {
        const auto result = settings::generate_invite_code();
        REQUIRE(result.is_error());
        CHECK(result.error().message.find("Windows") != std::string::npos);
        return;
    }
    std::set<std::string> seen;
    for (int i = 0; i < 50; ++i) {
        const auto code = settings::generate_invite_code();
        REQUIRE(code.is_ok());
        CHECK(code.value().size() == settings::kGeneratedInviteCodeLength);
        CHECK_FALSE(settings::check_invite_code(code.value()).has_value());
        seen.insert(code.value());
    }
    CHECK(seen.size() == 50);
}
