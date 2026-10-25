// Registro de addons conocidos: chatbot <nombre> y F2 usan la misma tabla.

#include "chatbot/addon_settings/registry.h"
#include "chatbot/addon_settings/telegram_section.h"

#include <catch2/catch_test_macros.hpp>

namespace settings = chatbot::addon_settings;

TEST_CASE("registro: solo telegram, con su programa, versión y servicio",
          "[addon_settings][registro]") {
    REQUIRE(settings::known_addons().size() == 1);
    const settings::AddonInfo* telegram = settings::find_addon("telegram");
    REQUIRE(telegram != nullptr);
    CHECK(telegram->title == "Telegram");
    CHECK(telegram->executable == "chatbot-telegram");
    CHECK(telegram->config_version == settings::kTelegramConfigVersion);
    CHECK(telegram->unit == "chatbot-telegram.service");
    for (const char* other : {"", "Telegram", "telegram ", "whatsapp", "../telegram"}) {
        CHECK(settings::find_addon(other) == nullptr);
    }
}
