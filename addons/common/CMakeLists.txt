# chatbot::bot: la lógica común de los bots (acceso, comandos, sesiones,
# límites, respuestas largas partidas y errores), sin red ni plataforma.
# Depende de chatbot::core, chatbot::markdown y chatbot::addon_settings (los
# rangos de la configuración y la zona horaria).
find_package(Threads REQUIRED)

add_library(chatbot_bot STATIC
    src/access.cpp
    src/bot.cpp
    src/commands.cpp
    src/config.cpp
    src/replies.cpp
    src/sessions.cpp
    src/split.cpp
    src/types.cpp)

add_library(chatbot::bot ALIAS chatbot_bot)

target_compile_features(chatbot_bot PUBLIC cxx_std_20)

target_include_directories(chatbot_bot PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

target_link_libraries(chatbot_bot
    PUBLIC chatbot::core chatbot::addon_settings Threads::Threads
    PRIVATE chatbot::markdown)

chatbot_set_warnings(chatbot_bot)

set_property(GLOBAL APPEND PROPERTY CHATBOT_ADDON_TARGETS chatbot_bot)

if(CHATBOT_BUILD_TESTS)
    add_subdirectory(tests)
endif()
