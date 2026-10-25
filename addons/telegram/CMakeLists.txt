# Addon de Telegram: cliente de la Bot API (long polling), mensajes
# enriquecidos con streaming, configuración y el ejecutable chatbot-telegram.
# Solo Linux por ahora (AGENTS.md, sección "Addons"). Depende de chatbot::bot,
# chatbot::core y chatbot::markdown; nunca de cli/ ni de FTXUI.
find_package(nlohmann_json 3.9 REQUIRED)
find_package(Threads REQUIRED)

add_library(chatbot_telegram STATIC
    src/api.cpp
    src/integer.cpp
    src/log.cpp
    src/outbox.cpp
    src/poller.cpp
    src/rich_markdown.cpp
    src/settings.cpp
    src/signal_watcher.cpp
    src/startup.cpp)

add_library(chatbot::telegram ALIAS chatbot_telegram)

target_compile_features(chatbot_telegram PUBLIC cxx_std_20)
target_include_directories(chatbot_telegram PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(chatbot_telegram
    PUBLIC chatbot::bot chatbot::core Threads::Threads
    PRIVATE chatbot::markdown nlohmann_json::nlohmann_json)
chatbot_set_warnings(chatbot_telegram)

# El bot: "chatbot telegram" lo arranca (README, "Bot de Telegram").
add_executable(chatbot-telegram src/main.cpp)
target_link_libraries(chatbot-telegram PRIVATE chatbot::telegram)
chatbot_set_warnings(chatbot-telegram)

# Sonda manual (como tools/smoke): manda mensajes de prueba a un chat para
# ver cómo los muestra Telegram. Nunca corre en las pruebas ni en la CI.
add_executable(chatbot-telegram-probe src/probe.cpp)
target_link_libraries(chatbot-telegram-probe PRIVATE chatbot::telegram)
chatbot_set_warnings(chatbot-telegram-probe)

set_property(GLOBAL APPEND PROPERTY CHATBOT_ADDON_TARGETS
    chatbot_telegram chatbot-telegram chatbot-telegram-probe)

# Junto a chatbot: así "chatbot telegram" lo encuentra en el build.
set_target_properties(chatbot-telegram chatbot-telegram-probe PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:chatbot>)

if(CHATBOT_BUILD_TESTS)
    add_subdirectory(tests)
endif()
