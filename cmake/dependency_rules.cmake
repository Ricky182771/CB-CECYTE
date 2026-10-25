# Dirección de las dependencias (AGENTS.md, secciones 5 y "Addons"): core,
# chatbot::markdown y los addons nunca dependen de cli/ ni de FTXUI. La
# revisión se hace al configurar, recorriendo las dependencias de enlace de
# cada target (también las transitivas y las privadas de las bibliotecas
# estáticas); si encuentra una prohibida, la configuración falla.

# Todos los targets de los que depende target, contando target, con los alias
# resueltos (ftxui::dom y dom aparecen los dos).
function(chatbot_collect_dependencies target out_var)
    set(pending ${target})
    set(seen "")
    while(pending)
        list(POP_FRONT pending current)
        if(current IN_LIST seen)
            continue()
        endif()
        list(APPEND seen ${current})
        if(NOT TARGET ${current})
            continue()
        endif()
        get_target_property(aliased ${current} ALIASED_TARGET)
        if(aliased)
            list(APPEND pending ${aliased})
            continue()
        endif()
        foreach(property LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
            get_target_property(libraries ${current} ${property})
            if(NOT libraries)
                continue()
            endif()
            foreach(library IN LISTS libraries)
                # Dependencias privadas de una biblioteca estática.
                string(REGEX REPLACE "^\\$<LINK_ONLY:(.+)>$" "\\1" library "${library}")
                # Otras expresiones de generador y las marcas ::@ que CMake
                # agrega cuando se enlaza desde otra carpeta.
                if(library MATCHES "\\$<" OR library MATCHES "^::@")
                    continue()
                endif()
                list(APPEND pending ${library})
            endforeach()
        endforeach()
    endwhile()
    set(${out_var} ${seen} PARENT_SCOPE)
endfunction()

# Falla la configuración si target depende de cli/ (chatbot_cli_lib,
# chatbot_cli_ui, el ejecutable chatbot) o de FTXUI.
function(chatbot_forbid_ui_dependencies target)
    set(forbidden chatbot_cli_lib chatbot_cli_ui chatbot)
    foreach(ftxui_target ftxui::screen ftxui::dom ftxui::component)
        if(TARGET ${ftxui_target})
            get_target_property(real ${ftxui_target} ALIASED_TARGET)
            list(APPEND forbidden ${ftxui_target})
            if(real)
                list(APPEND forbidden ${real})
            endif()
        endif()
    endforeach()
    chatbot_collect_dependencies(${target} dependencies)
    list(REMOVE_ITEM dependencies ${target})
    foreach(dependency IN LISTS dependencies)
        if(dependency IN_LIST forbidden OR dependency MATCHES "^ftxui::")
            message(FATAL_ERROR
                "${target} depende de ${dependency}: core, chatbot::markdown y los addons "
                "nunca dependen de cli/ ni de FTXUI (AGENTS.md, sección \"Addons\").")
        endif()
    endforeach()
endfunction()

# Falla la configuración si target (cli/, core o chatbot::markdown) depende
# de un addon: de addons/ solo se puede usar chatbot::addon_settings. Se
# llama al final, cuando ya existen los targets de CHATBOT_ADDON_TARGETS.
function(chatbot_forbid_addon_dependencies target)
    get_property(forbidden GLOBAL PROPERTY CHATBOT_ADDON_TARGETS)
    chatbot_collect_dependencies(${target} dependencies)
    list(REMOVE_ITEM dependencies ${target})
    foreach(dependency IN LISTS dependencies)
        if(dependency IN_LIST forbidden)
            message(FATAL_ERROR
                "${target} depende de ${dependency}: de addons/, cli/ solo puede usar "
                "chatbot::addon_settings (AGENTS.md, sección \"Addons\").")
        endif()
    endforeach()
endfunction()
