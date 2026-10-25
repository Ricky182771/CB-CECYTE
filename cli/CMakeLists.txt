# FTXUI solo la usa la interfaz (cli/); core/ no depende de ella (sección 5).
# Siempre por FetchContent: la v7.0.3 publicada más el parche del #1351 de
# upstream (cmake/patches/ftxui-7.0.3-drain-input.patch), que lee toda la
# entrada disponible en cada vuelta; sin él, un pegado de 200 KB tarda 26 s en
# entrar. Ninguna versión publicada lo trae, así que no se busca la FTXUI del
# sistema (ni la de MSYS2): saldría sin el arreglo.
# Cuando salga un release que incluya el #1351: se borran el parche, el
# PATCH_COMMAND y apply_patch.cmake (si no lo usa nadie más), GIT_TAG pasa al
# commit de esa versión y regresa, antes de FetchContent,
#   find_package(ftxui <esa versión> QUIET)
#   if(NOT ftxui_FOUND) ... endif()
# con ese mínimo (cubre el 7.0.2 de antes: hasta ahí App::PostEvent no era
# seguro entre hilos y aquí se llama desde el hilo de trabajo).
# Fijada al commit de la etiqueta v7.0.3 (el del release publicado), no a la
# etiqueta, para que el parche siempre caiga sobre el mismo código. Sin
# GIT_SHALLOW: CMake solo lo documenta con nombre de rama o etiqueta, y el
# clon completo pesa casi lo mismo (103 MB contra 101 MB: --no-single-branch
# trae la punta de todas las ramas).
find_package(Git REQUIRED)
include(FetchContent)
set(FTXUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_DOCS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    ftxui
    GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI.git
    GIT_TAG f921fad208912747c17d129a8ef75ec7624b6eec # v7.0.3
    PATCH_COMMAND "${CMAKE_COMMAND}"
        "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}"
        "-DPATCH_FILE=${PROJECT_SOURCE_DIR}/cmake/patches/ftxui-7.0.3-drain-input.patch"
        -P "${PROJECT_SOURCE_DIR}/cmake/apply_patch.cmake")
FetchContent_MakeAvailable(ftxui)

# Lógica de la conversación y del hilo de trabajo: solo depende del núcleo
# y de chatbot::markdown,
# sin FTXUI, para poder probarla sin terminal.
find_package(Threads REQUIRED)
# Portapapeles y programas externos: el lanzador (posix_spawnp) solo en
# POSIX; la API de Win32 solo en Windows.
if(WIN32)
    set(CHATBOT_CLIPBOARD_SOURCE src/clipboard_windows.cpp src/child_process_windows.cpp)
else()
    set(CHATBOT_CLIPBOARD_SOURCE src/clipboard_posix.cpp src/child_process_posix.cpp)
endif()
add_library(chatbot_cli_lib STATIC
    src/block_actions.cpp
    src/clipboard.cpp
    ${CHATBOT_CLIPBOARD_SOURCE}
    src/code_blocks.cpp
    src/command_menu.cpp
    src/command_parser.cpp
    src/commands.cpp
    src/conversation.cpp
    src/conversation_export.cpp
    src/conversation_list.cpp
    src/conversation_store.cpp
    src/downloads.cpp
    src/glyphs.cpp
    src/input_edit.cpp
    src/key_log.cpp
    src/models_loader.cpp
    src/paste.cpp
    src/provider_settings.cpp
    src/providers.cpp
    src/request_runner.cpp
    src/search_settings.cpp
    src/sidebar.cpp
    src/terminal_reply.cpp)
target_compile_features(chatbot_cli_lib PUBLIC cxx_std_20)
target_include_directories(chatbot_cli_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
find_package(nlohmann_json 3.9 REQUIRED)
target_link_libraries(chatbot_cli_lib
    PUBLIC chatbot::core chatbot::markdown Threads::Threads
    PRIVATE nlohmann_json::nlohmann_json)
chatbot_set_warnings(chatbot_cli_lib)
# Windows: portapapeles y ventana de solo mensajes (user32). Biblioteca del
# sistema, no una dependencia nueva.
if(WIN32)
    target_link_libraries(chatbot_cli_lib PRIVATE user32)
endif()

# Dibujo con FTXUI que no necesita la terminal (solo dom y screen), aparte de
# chatbot_cli_lib para que esta siga sin FTXUI. Las pruebas de la vista lo
# usan con ftxui::Screen.
add_library(chatbot_cli_ui STATIC
    src/command_menu_view.cpp
    src/history_view.cpp
    src/input_style.cpp
    src/list_style.cpp
    src/sidebar_view.cpp
    src/status_line.cpp
    src/markdown_view.cpp
    src/text_edit.cpp
    src/theme.cpp
    src/title_bar.cpp)
target_compile_features(chatbot_cli_ui PUBLIC cxx_std_20)
target_link_libraries(chatbot_cli_ui PUBLIC chatbot_cli_lib ftxui::dom ftxui::screen)
chatbot_set_warnings(chatbot_cli_ui)

# Ejecutable de la interfaz: el único que usa ftxui::component.
add_executable(chatbot src/main.cpp src/settings_screen.cpp src/text_area.cpp)
target_link_libraries(chatbot PRIVATE chatbot_cli_lib chatbot_cli_ui ftxui::component)
chatbot_set_warnings(chatbot)
chatbot_windows_manifest(chatbot)
