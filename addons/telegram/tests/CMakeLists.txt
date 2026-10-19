# Pruebas del addon de Telegram, sin red: FakeTransport con respuestas
# pregrabadas de la Bot API y reloj falso. Usan los helpers de tests/ y el
# arnés de addons/common/tests, nunca cli/.
add_executable(chatbot_telegram_tests
    test_api.cpp
    test_integration.cpp
    test_outbox.cpp
    test_poller.cpp
    test_rich_markdown.cpp)
target_compile_features(chatbot_telegram_tests PRIVATE cxx_std_20)
target_include_directories(chatbot_telegram_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/tests
    ${PROJECT_SOURCE_DIR}/addons/common/tests)
target_link_libraries(chatbot_telegram_tests PRIVATE
    chatbot::telegram chatbot::markdown Catch2::Catch2WithMain nlohmann_json::nlohmann_json)
chatbot_set_warnings(chatbot_telegram_tests)
add_test(NAME chatbot_telegram_tests COMMAND chatbot_telegram_tests)
set_tests_properties(chatbot_telegram_tests PROPERTIES LABELS addons)

set_property(GLOBAL APPEND PROPERTY CHATBOT_ADDON_TARGETS chatbot_telegram_tests)
