# chatbot::addon_settings: las reglas de la configuración de los addons en un
# solo lugar (rangos, sección de config.json, secretos, invitados, rutas y la
# unidad de systemd). La escribe la pantalla de configuración de la app y la
# lee el bot, así que se compila siempre, también con los addons apagados y
# en Windows (ahí lo que depende de POSIX responde "no disponible"). Solo
# depende de chatbot::core; sin red, sin FTXUI y sin hilos propios.
find_package(nlohmann_json 3.9 REQUIRED)

# Base de zonas horarias de C++20 (std::chrono::locate_zone y zoned_time)
# para el día del tope diario. libstdc++ 13 o posterior la tiene; libc++ (p.
# ej. Clang en Termux) puede no tenerla. AUTO la detecta compilando y
# enlazando una prueba (no por el nombre del compilador); OFF fuerza el
# respaldo con desfase fijo ("-06:00"), también para probarlo. En Windows,
# siempre el respaldo: ahí los addons no corren, y la prueba solo compila y
# enlaza (no sabe si al correr habría tzdata).
set(CHATBOT_TZDB "AUTO" CACHE STRING "Base de zonas de C++20: AUTO, ON u OFF")
set_property(CACHE CHATBOT_TZDB PROPERTY STRINGS AUTO ON OFF)
if(CHATBOT_TZDB STREQUAL "OFF" OR WIN32)
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
    message(STATUS "Addons: zona horaria con la base de zonas de C++20")
else()
    set(chatbot_zone_database src/zone_database_none.cpp)
    message(STATUS "Addons: sin base de zonas de C++20; zona horaria con desfase fijo")
endif()

# Lo que depende del sistema, en un archivo que elige CMake (sin #if en la
# lógica): getrandom y flock en POSIX; "no disponible" en Windows.
if(WIN32)
    set(chatbot_addon_platform src/invited_windows.cpp src/random_bytes_windows.cpp)
else()
    set(chatbot_addon_platform src/invited_posix.cpp src/random_bytes_posix.cpp)
endif()

add_library(chatbot_addon_settings STATIC
    src/bot_model.cpp
    src/invited.cpp
    src/limits.cpp
    src/paths.cpp
    src/registry.cpp
    src/secrets.cpp
    src/systemd_unit.cpp
    src/telegram_section.cpp
    src/time_zone.cpp
    src/token.cpp
    ${chatbot_zone_database}
    ${chatbot_addon_platform})

add_library(chatbot::addon_settings ALIAS chatbot_addon_settings)

target_compile_features(chatbot_addon_settings PUBLIC cxx_std_20)
target_include_directories(chatbot_addon_settings PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(chatbot_addon_settings
    PUBLIC chatbot::core
    PRIVATE nlohmann_json::nlohmann_json)
chatbot_set_warnings(chatbot_addon_settings)

if(CHATBOT_BUILD_TESTS)
    add_subdirectory(tests)
endif()
