# Aplica un parche a las fuentes de una dependencia de FetchContent sin fallar
# si ya estaba aplicado. Lo corre el PATCH_COMMAND de FetchContent, que trabaja
# en el directorio de las fuentes:
#
#   cmake -DGIT_EXECUTABLE=<git> -DPATCH_FILE=<archivo .patch> -P apply_patch.cmake
#
# - FetchContent repite el paso del parche en cada reconfiguración (depende del
#   paso "update" de git, que siempre corre) sobre las fuentes ya parcheadas:
#   un "git apply" a secas fallaría desde la segunda vez. Por eso primero se
#   revisa si aplica y, si no, si ya está puesto ("--reverse --check").
# - git apply, y no GNU patch ni un reemplazo de texto en CMake: git ya hace
#   falta en todas las plataformas para que FetchContent clone las
#   dependencias (Fedora, Termux, MSYS2), mientras que patch no viene en
#   Termux ni en MSYS2 sin instalarlo aparte. Además, git apply aplica todo o
#   nada y respeta los saltos de línea con los que git sacó los archivos.
cmake_minimum_required(VERSION 3.21)

foreach(variable IN ITEMS GIT_EXECUTABLE PATCH_FILE)
    if("${${variable}}" STREQUAL "")
        message(FATAL_ERROR "apply_patch.cmake: falta -D${variable}=...")
    endif()
endforeach()
if(NOT EXISTS "${PATCH_FILE}")
    message(FATAL_ERROR "No existe el parche ${PATCH_FILE}")
endif()
get_filename_component(patch_name "${PATCH_FILE}" NAME)
# En modo script, CMAKE_CURRENT_SOURCE_DIR es el directorio de trabajo.
set(sources "${CMAKE_CURRENT_SOURCE_DIR}")

# Las fuentes tienen que ser la raíz de su propio repositorio (FetchContent las
# clona con GIT_REPOSITORY). Si no, git apply usaría el repositorio que las
# contiene (build/ está dentro de este proyecto), tomaría las rutas del parche
# desde la raíz de ese repositorio y se saltaría los archivos sin avisar.
# --show-prefix da la ruta relativa a la raíz: vacía si estamos en ella.
execute_process(
    COMMAND "${GIT_EXECUTABLE}" rev-parse --show-prefix
    RESULT_VARIABLE result
    OUTPUT_VARIABLE prefix
    ERROR_VARIABLE error
    OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT result EQUAL 0 OR NOT prefix STREQUAL "")
    message(FATAL_ERROR
        "No se puede aplicar ${patch_name}: ${sources} no es la raíz de un "
        "repositorio de git. ${error}")
endif()

set(git_apply "${GIT_EXECUTABLE}" apply --whitespace=nowarn)

execute_process(
    COMMAND ${git_apply} --check "${PATCH_FILE}"
    RESULT_VARIABLE forward
    OUTPUT_QUIET
    ERROR_VARIABLE forward_error)
if(forward EQUAL 0)
    execute_process(
        COMMAND ${git_apply} "${PATCH_FILE}"
        RESULT_VARIABLE result
        ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "No se pudo aplicar ${patch_name} en ${sources}:\n${error}")
    endif()
    message(STATUS "Parche aplicado: ${patch_name}")
else()
    execute_process(
        COMMAND ${git_apply} --reverse --check "${PATCH_FILE}"
        RESULT_VARIABLE reverse
        OUTPUT_QUIET
        ERROR_QUIET)
    if(NOT reverse EQUAL 0)
        message(FATAL_ERROR
            "El parche ${patch_name} no aplica sobre ${sources} y tampoco está "
            "aplicado:\n${forward_error}\n"
            "Si cambió el parche o la versión de la dependencia, borra esa "
            "carpeta y su -subbuild (o todo build/<preset>) y vuelve a configurar.")
    endif()
    message(STATUS "El parche ya estaba aplicado: ${patch_name}")
endif()
