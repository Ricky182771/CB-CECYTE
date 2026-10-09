find_package(CURL REQUIRED)
find_package(nlohmann_json 3.9 REQUIRED)

add_library(chatbot_core STATIC
    src/atomic_file.cpp
    src/cancel_token.cpp
    src/credentials.cpp
    src/error.cpp
    src/error_body.cpp
    src/history.cpp
    src/config.cpp
    src/sleeper.cpp
    src/sse.cpp
    src/curl_transport.cpp
    src/chat_client.cpp
    src/url.cpp
    src/utf8.cpp
    src/web_search.cpp
    src/tavily_search.cpp)

add_library(chatbot::core ALIAS chatbot_core)

target_compile_features(chatbot_core PUBLIC cxx_std_20)

target_include_directories(chatbot_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

chatbot_set_warnings(chatbot_core)

# libcurl y nlohmann/json son detalles de implementación: no aparecen en los
# headers públicos, así que se enlazan en PRIVATE.
target_link_libraries(chatbot_core PRIVATE CURL::libcurl nlohmann_json::nlohmann_json)
