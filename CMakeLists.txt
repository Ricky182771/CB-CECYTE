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
endfunction()

option(CHATBOT_BUILD_TESTS "Compilar las pruebas (requiere Catch2 v3)" ON)

add_subdirectory(core)
add_subdirectory(cli)
add_subdirectory(tools)

if(CHATBOT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
