# Prueba del modo portable con el ejecutable real (cmake -P, sin shell):
# copia chatbot a una carpeta nueva con chatbot.portable a su lado y lo
# arranca sin key y sin terminal (stdin desde un archivo vacío), con HOME en
# una carpeta propia y sin CHAT_* ni XDG_*.
#
#   -DCHATBOT=<ruta del ejecutable> -DWORK=<carpeta de trabajo> -DMODE=<modo>
#
# MODE=writable: sale con 1 por la key que falta, crea app/datos (vacía, sin
#                el archivo de prueba) y no toca HOME.
# MODE=readonly: (solo POSIX) la carpeta del programa es de solo lectura:
#                sale con 1 con el error del modo portable, con la ruta, antes
#                de leer la configuración, y no toca HOME. Como root los
#                permisos no aplican: imprime "SKIP:" (SKIP_REGULAR_EXPRESSION).

foreach(variable CHATBOT WORK MODE)
    if(NOT DEFINED ${variable})
        message(FATAL_ERROR "Falta -D${variable}=...")
    endif()
endforeach()

# Restos de una corrida anterior (quizá de solo lectura).
if(EXISTS "${WORK}")
    file(CHMOD_RECURSE "${WORK}" DIRECTORY_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
         FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
    file(REMOVE_RECURSE "${WORK}")
endif()
set(app "${WORK}/app")
set(home "${WORK}/home")
file(MAKE_DIRECTORY "${app}" "${home}")
file(COPY "${CHATBOT}" DESTINATION "${app}")
get_filename_component(name "${CHATBOT}" NAME)
file(TOUCH "${app}/chatbot.portable")
file(WRITE "${WORK}/stdin.txt" "")

if(MODE STREQUAL "readonly")
    file(CHMOD "${app}" DIRECTORY_PERMISSIONS OWNER_READ OWNER_EXECUTE GROUP_READ
         GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E touch "${app}/escritura"
                    RESULT_VARIABLE writable OUTPUT_QUIET ERROR_QUIET)
    if(writable EQUAL 0)
        message("SKIP: se puede escribir en una carpeta de solo lectura (¿root?)")
        return()
    endif()
elseif(NOT MODE STREQUAL "writable")
    message(FATAL_ERROR "MODE desconocido: ${MODE}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        --unset=CHAT_API_KEY --unset=CHAT_BASE_URL --unset=CHAT_MODEL
        --unset=CHAT_TIMEOUT --unset=CHAT_HISTORY_LIMIT --unset=CHAT_DATA_DIR
        --unset=CHAT_DOWNLOAD_DIR --unset=CHAT_SEARCH_API_KEY --unset=CHAT_DEBUG_SSE
        --unset=CHAT_DEBUG_KEYS --unset=XDG_CONFIG_HOME --unset=XDG_DATA_HOME
        "HOME=${home}" "APPDATA=${home}/Roaming" "LOCALAPPDATA=${home}/Local"
        "${app}/${name}"
    INPUT_FILE "${WORK}/stdin.txt"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err)

# Antes de revisar: que la carpeta se pueda borrar aunque algo falle.
file(CHMOD "${app}" DIRECTORY_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
message("Salida de error del programa:\n${err}")

if(NOT status EQUAL 1)
    message(FATAL_ERROR "Se esperaba el código de salida 1 y fue ${status}")
endif()
file(GLOB home_entries "${home}/*" "${home}/.*")
if(home_entries)
    message(FATAL_ERROR "Se escribió en HOME: ${home_entries}")
endif()

if(MODE STREQUAL "writable")
    if(NOT err MATCHES "Falta la API key")
        message(FATAL_ERROR "Se esperaba el error de la key que falta")
    endif()
    if(err MATCHES "Modo portable")
        message(FATAL_ERROR "datos/ se podía escribir y aun así salió el error del modo portable")
    endif()
    if(NOT IS_DIRECTORY "${app}/datos")
        message(FATAL_ERROR "No se creó ${app}/datos")
    endif()
    file(GLOB datos_entries "${app}/datos/*" "${app}/datos/.*")
    if(datos_entries)
        message(FATAL_ERROR "Quedaron archivos en datos/: ${datos_entries}")
    endif()
else()
    if(NOT err MATCHES "Modo portable \\(chatbot.portable junto al ejecutable\\)")
        message(FATAL_ERROR "Se esperaba el error del modo portable")
    endif()
    string(FIND "${err}" "${app}/datos" path_at)
    string(FIND "${err}" "${app}/datos" last_path_at REVERSE)
    if(path_at EQUAL -1)
        message(FATAL_ERROR "El error no dice la ruta ${app}/datos")
    endif()
    if(NOT path_at EQUAL last_path_at)
        message(FATAL_ERROR "El error repite la ruta ${app}/datos")
    endif()
    if(err MATCHES "Falta la API key")
        message(FATAL_ERROR "Siguió hasta la configuración en lugar de salir")
    endif()
    if(EXISTS "${app}/datos")
        message(FATAL_ERROR "Se creó ${app}/datos en una carpeta de solo lectura")
    endif()
endif()

file(REMOVE_RECURSE "${WORK}")
