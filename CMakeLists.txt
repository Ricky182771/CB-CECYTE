cmake_minimum_required(VERSION 3.21)

project(chatbot
    VERSION 0.1.0
    DESCRIPTION "Chatbot de terminal: núcleo C++ (hito 1)"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Warnings exigentes del proyecto (sección 11). Se aplican por target mediante
# chatbot_set_warnings() y nunca a dependencias de terceros (p. ej. Catch2 vía
# FetchContent), que no es código nuestro y no compila limpio con -Werror.
function(chatbot_set_warnings target)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wconversion
            -Wshadow
            -Werror)
    endif()
    chatbot_windows_definitions(${target})
endfunction()

# Solo en Windows y solo para nuestros targets (chatbot_set_warnings los llama
# a todos): API de Windows 10 (_WIN32_WINNT=0x0A00), <windows.h> sin lo que
# no se usa y sin las macros min/max. <windows.h> solo se incluye en los .cpp
# de Windows, nunca en headers.
function(chatbot_windows_definitions target)
    if(WIN32)
        target_compile_definitions(${target} PRIVATE
            _WIN32_WINNT=0x0A00
            WIN32_LEAN_AND_MEAN
            NOMINMAX)
    endif()
endfunction()

# Manifiesto de Windows para un ejecutable (cmake/windows/chatbot.manifest):
# activeCodePage UTF-8, supportedOS de Windows 10 y 11 y asInvoker. Todos
# nuestros ejecutables lo llevan. GCC de MinGW enlaza además su manifiesto
# por defecto (default-manifest.o) si está instalado, como en MSYS2; no choca:
# ld lo descarta cuando hay otro manifiesto (probado con binutils 2.46).
function(chatbot_windows_manifest target)
    if(WIN32)
        target_sources(${target} PRIVATE ${PROJECT_SOURCE_DIR}/cmake/windows/manifest.rc)
    endif()
endfunction()

option(CHATBOT_BUILD_TESTS "Compilar las pruebas (requiere Catch2 v3)" ON)

# Addons (addons/, sección "Addons" de AGENTS.md): cada uno con su opción,
# apagada. Un build normal no compila ni descarga nada de ellos. Solo se
# prueban en Linux por ahora.
option(CHATBOT_BUILD_TELEGRAM "Compilar el addon de Telegram (solo Linux por ahora)" OFF)
set(CHATBOT_ADDON_OPTIONS CHATBOT_BUILD_TELEGRAM)
set(CHATBOT_ANY_ADDON OFF)
foreach(addon_option IN LISTS CHATBOT_ADDON_OPTIONS)
    if(${addon_option})
        set(CHATBOT_ANY_ADDON ON)
    endif()
endforeach()

include(cmake/dependency_rules.cmake)

if(CHATBOT_BUILD_TESTS)
    enable_testing()
    # Catch2 v3 es obligatoria (sección 4). Se usa la del sistema si existe
    # una v3; si no, FetchContent con versión fija (nunca master/main). Se
    # busca aquí para que la vean tests/ y las pruebas de addons/.
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
endif()

add_subdirectory(core)
add_subdirectory(markdown)
add_subdirectory(cli)
add_subdirectory(tools)

if(CHATBOT_BUILD_TESTS)
    add_subdirectory(tests)
endif()

# Después de cli/: la revisión de dependencias de los addons necesita que
# existan los targets de cli/ y de FTXUI para reconocerlos.
if(CHATBOT_ANY_ADDON)
    add_subdirectory(addons)
endif()

# core y chatbot::markdown nunca dependen de cli/ ni de FTXUI (sección 5).
chatbot_forbid_ui_dependencies(chatbot_core)
chatbot_forbid_ui_dependencies(chatbot_markdown)
