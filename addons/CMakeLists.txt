# Addons: programas aparte (bots) que usan el núcleo y chatbot::markdown,
# nunca cli/ ni FTXUI (AGENTS.md, sección "Addons"). Este directorio solo se
# incluye si algún addon está encendido (CHATBOT_BUILD_<ADDON>).

# Capa común de los bots (chatbot::bot): se compila con cualquier addon.
add_subdirectory(common)

# Telegram: cliente de la Bot API, mensajes enriquecidos y chatbot-telegram.
if(CHATBOT_BUILD_TELEGRAM)
    add_subdirectory(telegram)
endif()

# Ningún target de los addons depende de cli/ ni de FTXUI. Cada addon agrega
# los suyos a la propiedad global CHATBOT_ADDON_TARGETS.
get_property(addon_targets GLOBAL PROPERTY CHATBOT_ADDON_TARGETS)
foreach(addon_target IN LISTS addon_targets)
    chatbot_forbid_ui_dependencies(${addon_target})
endforeach()
