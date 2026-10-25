// F2 → Addons sin FTXUI: el error de cada campo (con las reglas del bot), la
// sección y las llaves de credentials.json que se escriben, y los textos del
// estado del addon y de su servicio.

#include "addon_form.h"

#include "chatbot/addon_settings/registry.h"
#include "chatbot/addon_settings/secrets.h"

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <vector>

namespace {

using chatbot::cli::AddonField;
using chatbot::cli::AddonForm;
using chatbot::cli::AddonSecrets;
namespace settings = chatbot::addon_settings;

constexpr const char* kToken = "123456789:AAFakeTokenForTestsOnly_abcdefghijk";
constexpr const char* kKey = "nvapi-clave-ficticia-de-prueba-1234";

AddonForm valid_form() {
    return chatbot::cli::form_from_section(settings::TelegramSection{});
}

std::optional<std::string> error_of(const AddonForm& form, AddonField field,
                                    const AddonSecrets& saved = {kToken, {}, {}}) {
    return chatbot::cli::addon_field_error(form, saved, field);
}

const settings::AddonInfo& telegram() {
    return *settings::find_addon("telegram");
}

} // namespace

TEST_CASE("addons: la sección va y vuelve por el formulario", "[addons][formulario]") {
    settings::TelegramSection section;
    section.enabled = true;
    section.model = "meta/llama";
    section.allowed_users = {111111111, 222222222};
    section.invite_code = "codigo-de-prueba";
    section.daily_message_limit = 30;
    section.max_concurrent_responses = 2;
    section.session_idle_minutes = 90;
    section.stale_message_minutes = 5;
    section.draft_interval_ms = 500;
    section.time_zone = "-06:00";
    section.log_error_details = true;
    const AddonForm form = chatbot::cli::form_from_section(section);
    CHECK(form.at(AddonField::AllowedUsers) == "111111111, 222222222");
    CHECK(form.at(AddonField::Token).empty());
    CHECK(chatbot::cli::section_from_form(form) == section);
    CHECK_FALSE(chatbot::cli::addon_form_error(form, {kToken, {}, {}}).has_value());

    AddonForm spaced = form;
    spaced.at(AddonField::AllowedUsers) = " 1,2 ,3 ";
    spaced.at(AddonField::DailyLimit) = " 7 ";
    spaced.at(AddonField::TimeZone) = "  ";
    const settings::TelegramSection read = chatbot::cli::section_from_form(spaced);
    CHECK(read.allowed_users == std::vector<std::int64_t>{1, 2, 3});
    CHECK(read.daily_message_limit == 7);
    CHECK(read.time_zone.empty());
}

TEST_CASE("addons: el error de cada campo", "[addons][formulario]") {
    AddonForm form = valid_form();
    for (std::size_t i = 0; i < chatbot::cli::kAddonFieldCount; ++i) {
        INFO(i);
        CHECK_FALSE(error_of(form, static_cast<AddonField>(i)).has_value());
    }

    SECTION("token") {
        form.at(AddonField::Token) = "https://api.telegram.org/x";
        CHECK(error_of(form, AddonField::Token).has_value());
        form.at(AddonField::Token).clear();
        form.enabled = true;
        CHECK_FALSE(error_of(form, AddonField::Token).has_value()); // El guardado.
        CHECK(error_of(form, AddonField::Token, {}) == "Falta el token de @BotFather.");
        form.at(AddonField::Token) = kToken;
        CHECK_FALSE(error_of(form, AddonField::Token, {}).has_value());
    }
    SECTION("keys propias") {
        form.at(AddonField::ModelKey) = "CHAT_API_KEY";
        CHECK(error_of(form, AddonField::ModelKey).has_value());
        form.at(AddonField::SearchKey) = "abc";
        CHECK(error_of(form, AddonField::SearchKey).has_value());
        form.at(AddonField::ModelKey) = kKey;
        CHECK_FALSE(error_of(form, AddonField::ModelKey).has_value());
    }
    SECTION("IDs") {
        form.at(AddonField::AllowedUsers) = "123, abc";
        CHECK(error_of(form, AddonField::AllowedUsers) ==
              "Escribe IDs numéricos separados por comas.");
        form.at(AddonField::AllowedUsers) = "1,,2";
        CHECK(error_of(form, AddonField::AllowedUsers).has_value());
        form.at(AddonField::AllowedUsers) = "0";
        CHECK(error_of(form, AddonField::AllowedUsers).has_value()); // check_allowed_users.
        form.at(AddonField::AllowedUsers) = "99999999999999999999";
        CHECK(error_of(form, AddonField::AllowedUsers).has_value());
    }
    SECTION("código") {
        form.at(AddonField::InviteCode) = "corto";
        const std::optional<std::string> error = error_of(form, AddonField::InviteCode);
        REQUIRE(error.has_value());
        CHECK(error->find("corto") == std::string::npos); // Nunca repite el código.
    }
    SECTION("números y rangos") {
        for (const AddonField field : {AddonField::DailyLimit, AddonField::Concurrent,
                                       AddonField::IdleMinutes, AddonField::StaleMinutes,
                                       AddonField::DraftMs}) {
            INFO(static_cast<int>(field));
            form.at(field) = "diez";
            CHECK(error_of(form, field) == "Escribe un número entero.");
            form.at(field) = "1.5";
            CHECK(error_of(form, field) == "Escribe un número entero.");
            form.at(field) = "0";
            CHECK(error_of(form, field).has_value());
        }
        form.at(AddonField::Concurrent) = "33";
        CHECK(error_of(form, AddonField::Concurrent).has_value());
        form.at(AddonField::Concurrent) = "32";
        CHECK_FALSE(error_of(form, AddonField::Concurrent).has_value());
    }
    SECTION("zona horaria") {
        form.at(AddonField::TimeZone) = "+05:30";
        CHECK_FALSE(error_of(form, AddonField::TimeZone).has_value());
        form.at(AddonField::TimeZone) = "Marte/Olympus";
        CHECK(error_of(form, AddonField::TimeZone).has_value());
    }
    SECTION("el primero, con su etiqueta") {
        form.at(AddonField::StaleMinutes) = "x";
        form.at(AddonField::DraftMs) = "x";
        CHECK(chatbot::cli::addon_form_error(form, {}) ==
              "Min. mensaje viejo: Escribe un número entero.");
    }
}

TEST_CASE("addons: las llaves de credentials.json", "[addons][formulario]") {
    AddonForm form = valid_form();
    CHECK(chatbot::cli::addon_credential_changes(form).empty());
    form.at(AddonField::Token) = kToken;
    form.at(AddonField::SearchKey) = kKey;
    form.remove_model_key = true;
    const std::vector<chatbot::cli::CredentialChange> changes =
        chatbot::cli::addon_credential_changes(form);
    REQUIRE(changes.size() == 3);
    CHECK(changes[0] == chatbot::cli::CredentialChange{std::string{settings::kTokenCredentialsKey},
                                                       std::string{kToken}});
    CHECK(changes[1] == chatbot::cli::CredentialChange{
                            std::string{settings::kBotApiKeyCredentialsKey}, std::nullopt});
    CHECK(changes[2] ==
          chatbot::cli::CredentialChange{std::string{settings::kBotSearchKeyCredentialsKey},
                                         std::string{kKey}});
}

TEST_CASE("addons: qué necesita reiniciar el bot", "[addons][formulario]") {
    const AddonForm saved = valid_form();
    const AddonSecrets secrets{kToken, {}, {}};
    AddonForm form = saved;
    form.at(AddonField::DailyLimit) = "10";
    CHECK_FALSE(chatbot::cli::addon_needs_restart(saved, secrets, form));
    form.at(AddonField::Token) = kToken; // El mismo.
    CHECK_FALSE(chatbot::cli::addon_needs_restart(saved, secrets, form));
    form.at(AddonField::Token) = "987654321:AAOtroTokenFicticioDePrueba_xyz12345";
    CHECK(chatbot::cli::addon_needs_restart(saved, secrets, form));
    form = saved;
    form.at(AddonField::Concurrent) = "8";
    CHECK(chatbot::cli::addon_needs_restart(saved, secrets, form));
}

TEST_CASE("addons: estado de los secretos y nota del entorno", "[addons][formulario]") {
    CHECK(chatbot::cli::addon_secret_status("", false) == "sin configurar");
    CHECK(chatbot::cli::addon_secret_status(kKey, false) == "guardada: …1234");
    CHECK(chatbot::cli::addon_secret_status(kKey, true) == "se quita: se usará la de la app");
    CHECK(chatbot::cli::addon_env_note({}).empty());
    CHECK(chatbot::cli::addon_env_note({settings::kApiKeyEnv}) ==
          "CHAT_API_KEY está definida en esta terminal, pero el servicio no la ve.");
    CHECK(chatbot::cli::addon_env_note(
              {settings::kTokenEnv, settings::kApiKeyEnv, settings::kSearchKeyEnv}) ==
          "CHAT_TELEGRAM_TOKEN, CHAT_API_KEY y CHAT_SEARCH_API_KEY están definidas en esta "
          "terminal, pero el servicio no las ve.");
}

TEST_CASE("addons: instalado, no instalado e incompatible", "[addons][estado]") {
    using chatbot::cli::AddonInstall;
    CHECK(chatbot::cli::addon_install_problem(telegram(), AddonInstall{}, false) ==
          "No instalado: copia chatbot-telegram junto a chatbot o en el PATH.");
    CHECK(chatbot::cli::addon_install_problem(telegram(), AddonInstall{}, true) ==
          "No instalado: copia chatbot-telegram junto a chatbot.");
    CHECK(chatbot::cli::addon_install_problem(telegram(), AddonInstall{"/x/chatbot-telegram", 1},
                                              false)
              .empty());
    CHECK(chatbot::cli::addon_install_problem(telegram(), AddonInstall{"/x/chatbot-telegram", 2},
                                              false) ==
          "Incompatible: esta app usa la versión 1 de la configuración y chatbot-telegram "
          "entiende la 2.");
    CHECK(chatbot::cli::addon_install_problem(
              telegram(), AddonInstall{"/x/chatbot-telegram", std::nullopt}, false)
              .starts_with("Incompatible: "));

    chatbot::cli::ProcessResult result;
    result.status = chatbot::cli::ProcessResult::Status::Exited;
    result.exit_code = 0;
    result.out = "1\n";
    CHECK(chatbot::cli::parse_config_version(result) == 1);
    result.out = "1 2\n";
    CHECK_FALSE(chatbot::cli::parse_config_version(result).has_value());
    result.out = "1\n";
    result.exit_code = 2;
    CHECK_FALSE(chatbot::cli::parse_config_version(result).has_value());
}

TEST_CASE("addons: el resumen y el error del servicio", "[addons][estado]") {
    chatbot::cli::ServiceStatus status;
    status.systemd = false;
    CHECK(chatbot::cli::service_summary(status) == chatbot::cli::kNoUserSystemd);
    status = {};
    status.enabled = "not-found";
    CHECK(chatbot::cli::service_summary(status) == "Servicio: no instalado");
    status.unit = settings::UnitState::Current;
    status.active = "active";
    status.enabled = "enabled";
    CHECK(chatbot::cli::service_summary(status) == "Servicio: activo · habilitado");
    status.unit = settings::UnitState::Foreign;
    CHECK(chatbot::cli::service_summary(status) == "Servicio: activo · habilitado · hecho a mano");

    status.last_error = "telegram_token_invalido http=401";
    CHECK(chatbot::cli::service_problem(status, "chatbot-telegram", "/d").empty()); // Corre.
    status.active = "failed";
    CHECK(chatbot::cli::service_problem(status, "chatbot-telegram", "/d") ==
          "Último error: telegram_token_invalido http=401");
    status.exit_status = 65;
    CHECK(chatbot::cli::service_problem(status, "chatbot-telegram", "/d") ==
          "invitados.json está dañado: /d/invitados.json. Arréglalo o quítalo de ahí.");
    status.exit_status = 73;
    CHECK(chatbot::cli::service_problem(status, "chatbot-telegram", "/d") ==
          "No se puede escribir en la carpeta de datos del bot: /d.");
    status.exit_status = 1;
    CHECK(chatbot::cli::service_problem(status, "chatbot-telegram", "/d") ==
          "Último error: telegram_token_invalido http=401");
    status.last_error.clear();
    CHECK(chatbot::cli::service_problem(status, "chatbot-telegram", "/d") ==
          "Salió con el código 1.");
    CHECK(chatbot::cli::journal_command(telegram()) ==
          "journalctl --user -u chatbot-telegram -f");
}

TEST_CASE("addons: el aviso cuando el servicio no está como dice Activado", "[addons][estado]") {
    using chatbot::cli::service_mismatch;
    chatbot::cli::ServiceStatus status;
    status.unit = settings::UnitState::Current;
    status.active = "active";
    status.enabled = "enabled";
    CHECK(service_mismatch(status, true).empty());
    CHECK(service_mismatch(status, false) ==
          "Desactivado, pero el servicio sigue corriendo: guarda para detenerlo.");

    status.active = "inactive";
    CHECK(service_mismatch(status, true) ==
          "Activado, pero el servicio está detenido: guarda para arrancarlo.");
    CHECK(service_mismatch(status, false) ==
          "Desactivado, pero el servicio sigue habilitado: guarda para deshabilitarlo.");
    status.active = "failed";
    CHECK(service_mismatch(status, true) ==
          "Activado, pero el servicio falló: guarda para arrancarlo.");
    status.enabled = "disabled";
    CHECK(service_mismatch(status, false).empty());
    status.active = "active";
    CHECK(service_mismatch(status, true) ==
          "Activado, pero el servicio está deshabilitado: guarda para habilitarlo.");

    status.enabled = "enabled";
    status.unit = settings::UnitState::Missing;
    CHECK(service_mismatch(status, true) ==
          "Activado, pero el servicio no está instalado: guarda para instalarlo.");
    status.unit = settings::UnitState::Foreign;
    CHECK(service_mismatch(status, true) ==
          "Activado, pero el servicio está hecho a mano: guarda para reemplazarlo.");
    status.unit = settings::UnitState::Stale;
    CHECK(service_mismatch(status, true) ==
          "Activado, pero el servicio está desactualizado: guarda para actualizarlo.");

    status.systemd = false;
    CHECK(service_mismatch(status, true).empty());
}
