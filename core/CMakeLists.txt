find_package(CURL REQUIRED)
find_package(nlohmann_json 3.2 REQUIRED)

add_library(chatbot_core STATIC
    src/error.cpp
    src/config.cpp
    src/sleeper.cpp
    src/curl_transport.cpp
    src/chat_client.cpp)

add_library(chatbot::core ALIAS chatbot_core)

target_compile_features(chatbot_core PUBLIC cxx_std_20)

target_include_directories(chatbot_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

chatbot_set_warnings(chatbot_core)

# libcurl y nlohmann/json son detalles de implementación: no aparecen en los
# headers públicos, así que se enlazan en PRIVATE.
target_link_libraries(chatbot_core PRIVATE CURL::libcurl nlohmann_json::nlohmann_json)
