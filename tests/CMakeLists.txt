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

find_package(nlohmann_json 3.2 REQUIRED)

add_executable(chatbot_tests
    test_config.cpp
    test_conversation.cpp
    test_error_mapping.cpp
    test_response.cpp
    test_retries.cpp
    test_request_body.cpp
    test_sse.cpp
    test_streaming.cpp)

target_compile_features(chatbot_tests PRIVATE cxx_std_20)
target_include_directories(chatbot_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/core/src)
target_link_libraries(chatbot_tests PRIVATE
    chatbot::core chatbot_cli_lib Catch2::Catch2WithMain nlohmann_json::nlohmann_json)

chatbot_set_warnings(chatbot_tests)

add_test(NAME chatbot_tests COMMAND chatbot_tests)
