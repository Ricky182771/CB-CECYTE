# Addon de Telegram: cliente de la Bot API (long polling), mensajes
# enriquecidos con streaming, configuración y el ejecutable chatbot-telegram.
# Solo Linux por ahora (AGENTS.md, sección "Addons"). Depende de chatbot::bot,
# chatbot::core y chatbot::markdown; nunca de cli/ ni de FTXUI.
find_package(nlohmann_json 3.9 REQUIRED)
find_package(Threads REQUIRED)

add_library(chatbot_telegram STATIC
    src/rich_markdown.cpp)

add_library(chatbot::telegram ALIAS chatbot_telegram)

target_compile_features(chatbot_telegram PUBLIC cxx_std_20)
target_include_directories(chatbot_telegram PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(chatbot_telegram
    PUBLIC chatbot::bot chatbot::core Threads::Threads
    PRIVATE chatbot::markdown nlohmann_json::nlohmann_json)
chatbot_set_warnings(chatbot_telegram)

set_property(GLOBAL APPEND PROPERTY CHATBOT_ADDON_TARGETS chatbot_telegram)

if(CHATBOT_BUILD_TESTS)
    add_subdirectory(tests)
endif()
