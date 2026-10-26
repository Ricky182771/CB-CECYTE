# Dirección de las dependencias en los #include (AGENTS.md, sección
# "Addons"). La revisión del enlace la hace cmake/dependency_rules.cmake al
# configurar; esta revisa el código fuente, también con los addons apagados:
#   cmake -DSOURCE_DIR=<raíz del proyecto> -P include_rules.cmake
# - core/, markdown/, cli/, rpc/, tools/ y tests/ nunca incluyen nada de
#   addons/ (chatbot/bot/..., chatbot/telegram/... ni rutas con addons/),
#   salvo chatbot/addon_settings/... en cli/, rpc/, tools/ y tests/.
# - core/ y markdown/ tampoco incluyen chatbot/addon_settings/ (está encima).
# - addons/settings/ no incluye chatbot/bot/ ni chatbot/telegram/ (está debajo).
# - markdown/ y addons/ nunca incluyen FTXUI ni headers de cli/src.
# - rpc/ nunca incluye FTXUI (sí los headers de chatbot_cli_lib).
# Imprime cada violación con archivo y línea, y falla si hay alguna.

if(NOT DEFINED SOURCE_DIR)
    message(FATAL_ERROR "Falta -DSOURCE_DIR=<raíz del proyecto>")
endif()

set(violations "")

# Los #include de un archivo: lista de "línea|ruta|comillas" (comillas es 1
# para "...", 0 para <...>).
function(read_includes file out_var)
    # Una línea por elemento: sin los ';', las '\' ni los corchetes del
    # código, que partirían o juntarían los elementos de la lista de CMake.
    file(READ "${file}" content)
    string(REPLACE "\\" "/" content "${content}")
    string(REPLACE ";" "," content "${content}")
    string(REPLACE "[" "(" content "${content}")
    string(REPLACE "]" ")" content "${content}")
    string(REPLACE "\n" ";" lines "${content}")
    set(result "")
    set(number 0)
    foreach(line IN LISTS lines)
        math(EXPR number "${number} + 1")
        if(line MATCHES "^[ \t]*#[ \t]*include[ \t]*\"([^\"]+)\"")
            list(APPEND result "${number}|${CMAKE_MATCH_1}|1")
        elseif(line MATCHES "^[ \t]*#[ \t]*include[ \t]*<([^>]+)>")
            list(APPEND result "${number}|${CMAKE_MATCH_1}|0")
        endif()
    endforeach()
    set(${out_var} "${result}" PARENT_SCOPE)
endfunction()

function(sources_under directory out_var)
    file(GLOB_RECURSE found LIST_DIRECTORIES false
        "${SOURCE_DIR}/${directory}/*.h" "${SOURCE_DIR}/${directory}/*.hpp"
        "${SOURCE_DIR}/${directory}/*.cpp" "${SOURCE_DIR}/${directory}/*.c")
    set(${out_var} "${found}" PARENT_SCOPE)
endfunction()

macro(add_violation file number text)
    file(RELATIVE_PATH relative "${SOURCE_DIR}" "${file}")
    list(APPEND violations "${relative}:${number}: ${text}")
endmacro()

# 1. Nada fuera de addons/ incluye algo de addons/.
foreach(directory core markdown cli rpc tools tests)
    sources_under(${directory} files)
    foreach(file IN LISTS files)
        read_includes("${file}" includes)
        foreach(include IN LISTS includes)
            string(REPLACE "|" ";" parts "${include}")
            list(GET parts 0 number)
            list(GET parts 1 path)
            if(path MATCHES "^chatbot/(bot|telegram)/" OR path MATCHES "(^|/)addons/")
                add_violation("${file}" ${number} "incluye ${path}, que es de addons/")
            elseif(path MATCHES "^chatbot/addon_settings/" AND directory MATCHES "^(core|markdown)$")
                add_violation("${file}" ${number} "incluye ${path}, que está encima de ${directory}/")
            endif()
        endforeach()
    endforeach()
endforeach()

# 2. addons/settings/ no incluye la capa de los bots ni a Telegram.
sources_under(addons/settings files)
foreach(file IN LISTS files)
    read_includes("${file}" includes)
    foreach(include IN LISTS includes)
        string(REPLACE "|" ";" parts "${include}")
        list(GET parts 0 number)
        list(GET parts 1 path)
        if(path MATCHES "^chatbot/(bot|telegram)/")
            add_violation("${file}" ${number} "incluye ${path}: addons/settings está debajo")
        endif()
    endforeach()
endforeach()

# 3. markdown/ y addons/ no incluyen FTXUI ni headers de cli/src.
foreach(directory markdown addons)
    if(NOT IS_DIRECTORY "${SOURCE_DIR}/${directory}")
        continue()
    endif()
    sources_under(${directory} files)
    foreach(file IN LISTS files)
        get_filename_component(here "${file}" DIRECTORY)
        read_includes("${file}" includes)
        foreach(include IN LISTS includes)
            string(REPLACE "|" ";" parts "${include}")
            list(GET parts 0 number)
            list(GET parts 1 path)
            list(GET parts 2 quoted)
            if(path MATCHES "^ftxui/")
                add_violation("${file}" ${number} "incluye ${path} (FTXUI)")
            elseif(path MATCHES "(^|/)cli/")
                add_violation("${file}" ${number} "incluye ${path}, que es de cli/")
            elseif(quoted AND EXISTS "${SOURCE_DIR}/cli/src/${path}" AND NOT EXISTS "${here}/${path}")
                add_violation("${file}" ${number} "incluye ${path}, un header de cli/src")
            endif()
        endforeach()
    endforeach()
endforeach()

# 4. rpc/ no incluye FTXUI.
sources_under(rpc files)
foreach(file IN LISTS files)
    read_includes("${file}" includes)
    foreach(include IN LISTS includes)
        string(REPLACE "|" ";" parts "${include}")
        list(GET parts 0 number)
        list(GET parts 1 path)
        if(path MATCHES "^ftxui/")
            add_violation("${file}" ${number} "incluye ${path} (FTXUI)")
        endif()
    endforeach()
endforeach()

if(violations)
    foreach(violation IN LISTS violations)
        message(STATUS "${violation}")
    endforeach()
    list(LENGTH violations count)
    message(FATAL_ERROR "${count} #include contra la dirección de las dependencias")
endif()
message(STATUS "Dirección de las dependencias en los #include: bien")
