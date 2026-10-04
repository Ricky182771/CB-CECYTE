# Catch2 v3 es obligatoria (sección 4). Se usa la del sistema si existe una
# v3; si no, FetchContent con versión fija (nunca master/main).
find_package(Catch2 3 QUIET COMPONENTS Catch2WithMain)
if(NOT Catch2_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.16.0
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(Catch2)
endif()

find_package(nlohmann_json 3.9 REQUIRED)

add_executable(chatbot_tests
    test_cancel.cpp
    test_config.cpp
    test_conversation.cpp
    test_conversation_list.cpp
    test_conversation_store.cpp
    test_debug_sse.cpp
    test_error_mapping.cpp
    test_history.cpp
    test_markdown.cpp
    test_response.cpp
    test_retries.cpp
    test_request_body.cpp
    test_request_runner.cpp
    test_sse.cpp
    test_streaming.cpp)

target_compile_features(chatbot_tests PRIVATE cxx_std_20)
target_include_directories(chatbot_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/core/src)
target_link_libraries(chatbot_tests PRIVATE
    chatbot::core chatbot_cli_lib Catch2::Catch2WithMain nlohmann_json::nlohmann_json)

chatbot_set_warnings(chatbot_tests)

add_test(NAME chatbot_tests COMMAND chatbot_tests)

# Pruebas de la vista de markdown: dibujan con ftxui::Screen (sin terminal).
# Van aparte para que chatbot_tests no enlace FTXUI.
add_executable(chatbot_ui_tests
    test_history_view.cpp
    test_markdown_view.cpp)
target_compile_features(chatbot_ui_tests PRIVATE cxx_std_20)
target_link_libraries(chatbot_ui_tests PRIVATE chatbot_cli_ui Catch2::Catch2WithMain)
# Archivos de tests/data (solo lectura).
target_compile_definitions(chatbot_ui_tests PRIVATE
    CHATBOT_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
chatbot_set_warnings(chatbot_ui_tests)
add_test(NAME chatbot_ui_tests COMMAND chatbot_ui_tests)
