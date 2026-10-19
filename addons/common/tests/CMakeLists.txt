# Pruebas de chatbot::bot, sin red: FakeTransport, transportes que arman la
# respuesta a partir de la petición y un reloj falso. Usan los helpers de
# tests/ (fake_transport.hpp, blocking_transport.hpp...), nunca cli/.
find_package(nlohmann_json 3.9 REQUIRED)

add_executable(chatbot_bot_tests
    test_bot.cpp
    test_bot_access.cpp
    test_bot_commands.cpp
    test_bot_concurrency.cpp
    test_bot_config.cpp
    test_bot_limits.cpp
    test_bot_replies.cpp
    test_bot_search.cpp
    test_bot_sessions.cpp
    test_bot_split.cpp
    test_bot_streaming.cpp
    test_bot_time_zone.cpp)
target_compile_features(chatbot_bot_tests PRIVATE cxx_std_20)
target_include_directories(chatbot_bot_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/tests)
target_link_libraries(chatbot_bot_tests PRIVATE
    chatbot::bot chatbot::markdown Catch2::Catch2WithMain nlohmann_json::nlohmann_json)
chatbot_set_warnings(chatbot_bot_tests)
add_test(NAME chatbot_bot_tests COMMAND chatbot_bot_tests)
set_tests_properties(chatbot_bot_tests PROPERTIES LABELS addons)

set_property(GLOBAL APPEND PROPERTY CHATBOT_ADDON_TARGETS chatbot_bot_tests)
