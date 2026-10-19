# chatbot::bot: la lógica común de los bots (acceso, comandos, sesiones,
# límites, respuestas largas partidas y errores), sin red ni plataforma.
# Depende solo de chatbot::core y chatbot::markdown.
find_package(Threads REQUIRED)

# Base de zonas horarias de C++20 (std::chrono::locate_zone y zoned_time)
# para el día del tope diario. libstdc++ 13 o posterior la tiene; libc++ (p.
# ej. Clang en Termux) puede no tenerla. AUTO la detecta compilando y
# enlazando una prueba (no por el nombre del compilador); OFF fuerza el
# respaldo con desfase fijo ("-06:00"), también para probarlo.
set(CHATBOT_TZDB "AUTO" CACHE STRING "Base de zonas de C++20: AUTO, ON u OFF")
set_property(CACHE CHATBOT_TZDB PROPERTY STRINGS AUTO ON OFF)
if(CHATBOT_TZDB STREQUAL "OFF")
    set(chatbot_has_tzdb OFF)
else()
    include(CheckCXXSourceCompiles)
    check_cxx_source_compiles([=[
#include <chrono>
int main() {
    const std::chrono::time_zone* zone = std::chrono::locate_zone("UTC");
    const std::chrono::zoned_time local{zone, std::chrono::sys_seconds{}};
    return local.get_local_time().time_since_epoch().count() == 0 ? 0 : 1;
}
]=] CHATBOT_TZDB_WORKS)
    if(CHATBOT_TZDB_WORKS)
        set(chatbot_has_tzdb ON)
    elseif(CHATBOT_TZDB STREQUAL "ON")
        message(FATAL_ERROR "CHATBOT_TZDB=ON, pero esta biblioteca de C++ no tiene "
                            "std::chrono::locate_zone")
    else()
        set(chatbot_has_tzdb OFF)
    endif()
endif()
if(chatbot_has_tzdb)
    set(chatbot_zone_database src/zone_database_tzdb.cpp)
    message(STATUS "Bots: zona horaria con la base de zonas de C++20")
else()
    set(chatbot_zone_database src/zone_database_none.cpp)
    message(STATUS "Bots: sin base de zonas de C++20; zona horaria con desfase fijo")
endif()

add_library(chatbot_bot STATIC
    src/access.cpp
    src/bot.cpp
    src/commands.cpp
    src/config.cpp
    src/replies.cpp
    src/sessions.cpp
    src/split.cpp
    src/time_zone.cpp
    src/types.cpp
    ${chatbot_zone_database})

add_library(chatbot::bot ALIAS chatbot_bot)

target_compile_features(chatbot_bot PUBLIC cxx_std_20)

target_include_directories(chatbot_bot PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

target_link_libraries(chatbot_bot
    PUBLIC chatbot::core Threads::Threads
    PRIVATE chatbot::markdown)

chatbot_set_warnings(chatbot_bot)

set_property(GLOBAL APPEND PROPERTY CHATBOT_ADDON_TARGETS chatbot_bot)

if(CHATBOT_BUILD_TESTS)
    add_subdirectory(tests)
endif()
