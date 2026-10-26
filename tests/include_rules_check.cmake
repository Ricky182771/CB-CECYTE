# Revisa que include_rules.cmake falle de verdad (cmake -P): arma árboles de
# mentira con un #include prohibido y espera el error con su archivo; uno con
# lo permitido debe pasar.
#
#   -DRULES=<ruta de include_rules.cmake> -DWORK=<carpeta de trabajo>

foreach(variable RULES WORK)
    if(NOT DEFINED ${variable})
        message(FATAL_ERROR "Falta -D${variable}=...")
    endif()
endforeach()

# Un árbol con un solo archivo (relative) que incluye include; expected: 0
# si debe pasar, 1 si debe fallar.
function(check name relative include expected)
    set(root "${WORK}/${name}")
    file(REMOVE_RECURSE "${root}")
    file(WRITE "${root}/${relative}" "#include \"${include}\"\n")
    execute_process(COMMAND "${CMAKE_COMMAND}" -DSOURCE_DIR=${root} -P "${RULES}"
                    RESULT_VARIABLE status OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(expected EQUAL 0 AND NOT status EQUAL 0)
        message(FATAL_ERROR "${name}: ${relative} con ${include} debía pasar:\n${out}${err}")
    endif()
    if(expected EQUAL 1)
        string(FIND "${out}${err}" "${relative}:1:" found)
        if(status EQUAL 0 OR found EQUAL -1)
            message(FATAL_ERROR "${name}: ${relative} con ${include} debía fallar:\n${out}${err}")
        endif()
    endif()
endfunction()

check(cli_bot cli/src/a.cpp chatbot/bot/bot.h 1)
check(cli_telegram cli/src/a.cpp chatbot/telegram/api.h 1)
check(core_settings core/src/a.cpp chatbot/addon_settings/registry.h 1)
check(settings_bot addons/settings/src/a.cpp chatbot/bot/types.h 1)
check(cli_settings cli/src/a.cpp chatbot/addon_settings/registry.h 0)
check(rpc_ftxui rpc/src/a.cpp ftxui/dom/elements.hpp 1)
check(rpc_bot rpc/src/a.cpp chatbot/bot/bot.h 1)
check(rpc_cli rpc/src/a.cpp session.h 0)

message(STATUS "include_rules.cmake detecta los #include prohibidos")
