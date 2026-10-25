# chatbot <addon> con el ejecutable real (cmake -P, sin red): copia chatbot a
# carpetas nuevas y un chatbot-telegram falso (un script que imprime sus
# argumentos, uno por línea entre < >) junto a él o en el PATH.
#
#   -DCHATBOT=<ruta del ejecutable> -DWORK=<carpeta de trabajo>
#
# - Junto a chatbot: lo arranca con los argumentos tal cual (también con
#   espacios, vacíos y $), y su código de salida es el del addon.
# - En el PATH: lo arranca.
# - Modo portable (chatbot.portable): no busca en el PATH → 69, "junto a
#   chatbot." sin "ni en el PATH".
# - Sin addon: 69 con el mensaje. Argumento desconocido: 2 con el uso.
# - Windows: 1 con "Los addons todavía no están disponibles en Windows.".

foreach(variable CHATBOT WORK)
    if(NOT DEFINED ${variable})
        message(FATAL_ERROR "Falta -D${variable}=...")
    endif()
endforeach()

file(REMOVE_RECURSE "${WORK}")
get_filename_component(name "${CHATBOT}" NAME)
set(empty_path "${WORK}/vacia")
file(MAKE_DIRECTORY "${empty_path}")

# Copia chatbot a WORK/<folder> y regresa la ruta de la copia.
function(copy_chatbot folder out_var)
    file(MAKE_DIRECTORY "${WORK}/${folder}")
    file(COPY "${CHATBOT}" DESTINATION "${WORK}/${folder}")
    set(${out_var} "${WORK}/${folder}/${name}" PARENT_SCOPE)
endfunction()

# El addon falso: imprime cada argumento entre < > y sale con 7.
function(fake_addon folder)
    file(MAKE_DIRECTORY "${WORK}/${folder}")
    file(WRITE "${WORK}/${folder}/chatbot-telegram"
         "#!/bin/sh\nfor a in \"$@\"; do printf '<%s>\\n' \"$a\"; done\nexit 7\n")
    file(CHMOD "${WORK}/${folder}/chatbot-telegram" PERMISSIONS OWNER_READ OWNER_WRITE
         OWNER_EXECUTE)
endfunction()

# Corre program con PATH=path y el resto de los argumentos; deja status, out
# y err en el padre.
function(run program path)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env "PATH=${path}" --unset=XDG_CONFIG_HOME
            "HOME=${WORK}/home" "${program}" ${ARGN}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error)
    set(status "${result}" PARENT_SCOPE)
    set(out "${output}" PARENT_SCOPE)
    set(err "${error}" PARENT_SCOPE)
endfunction()

function(expect what expected)
    if(NOT status STREQUAL "${expected}")
        message(FATAL_ERROR "${what}: se esperaba el código ${expected} y fue ${status}\n"
                            "stdout:\n${out}\nstderr:\n${err}")
    endif()
endfunction()

function(expect_text what haystack needle)
    string(FIND "${haystack}" "${needle}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "${what}: no aparece \"${needle}\" en:\n${haystack}")
    endif()
endfunction()

if(CMAKE_HOST_WIN32)
    # Con el PATH de siempre: sin él, chatbot.exe no encuentra las DLL de MSYS2.
    copy_chatbot(windows chatbot)
    execute_process(COMMAND "${chatbot}" telegram RESULT_VARIABLE status OUTPUT_VARIABLE out
                    ERROR_VARIABLE err)
    expect("Windows" 1)
    expect_text("Windows" "${err}" "Los addons todavía no están disponibles en Windows.")
    return()
endif()

# Junto a chatbot, con argumentos difíciles; el PATH no tiene nada.
copy_chatbot(junto chatbot)
fake_addon(junto)
# Aquí sin run(): ${ARGN} tira los argumentos vacíos.
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "PATH=${empty_path}" "${chatbot}" telegram "--help" "a b"
        "" "$HOME" "--config-version"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err)
expect("junto a chatbot" 7)
set(expected_out "<--help>\n<a b>\n<>\n<$HOME>\n<--config-version>\n")
if(NOT out STREQUAL expected_out)
    message(FATAL_ERROR "junto a chatbot: argumentos distintos:\n${out}")
endif()

# En el PATH.
copy_chatbot(solo chatbot)
fake_addon(en_path)
run("${chatbot}" "${WORK}/en_path" telegram uno)
expect("en el PATH" 7)
if(NOT out STREQUAL "<uno>\n")
    message(FATAL_ERROR "en el PATH: argumentos distintos:\n${out}")
endif()
run("${chatbot}" "${WORK}/en_path" --help)
expect("--help con el addon en el PATH" 0)
expect_text("--help" "${out}" "Telegram: instalado en ${WORK}/en_path/chatbot-telegram")

# Modo portable: el del PATH no cuenta.
copy_chatbot(portable chatbot)
file(TOUCH "${WORK}/portable/chatbot.portable")
run("${chatbot}" "${WORK}/en_path" telegram)
expect("portable" 69)
expect_text("portable" "${err}" "No encontré chatbot-telegram junto a chatbot.")
fake_addon(portable)
run("${chatbot}" "${WORK}/en_path" telegram dos)
expect("portable con el addon junto" 7)

# Sin addon (en la carpeta del build sí está: va junto a chatbot).
copy_chatbot(sin_addon chatbot)
run("${chatbot}" "${empty_path}" telegram)
expect("sin addon" 69)
expect_text("sin addon" "${err}" "No encontré chatbot-telegram junto a chatbot ni en el PATH.")

# Argumento desconocido.
run("${chatbot}" "${empty_path}" --nada)
expect("argumento desconocido" 2)
expect_text("argumento desconocido" "${err}" "argumento desconocido: --nada")
expect_text("argumento desconocido" "${err}" "Uso:")

message(STATUS "chatbot <addon>: bien")
