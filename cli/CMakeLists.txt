# FTXUI solo la usa la interfaz (cli/); core/ no depende de ella (sección 5).
# Mínimo 7.0.2: antes, App::PostEvent no era seguro entre hilos y aquí se
# llama desde el hilo de trabajo. FTXUI declara SameMajorVersion, así que esto
# acepta cualquier 7.x >= 7.0.2. Si no está instalada, FetchContent con
# versión fija (nunca master/main).
find_package(ftxui 7.0.2 QUIET)
if(NOT ftxui_FOUND)
    include(FetchContent)
    set(FTXUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(FTXUI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(FTXUI_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        ftxui
        GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI.git
        GIT_TAG v7.0.3
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(ftxui)
endif()

# md4c (parser de markdown en C, MIT). Siempre por FetchContent con versión
# fija: su configuración de CMake no instala un archivo de versión, así que
# find_package no puede garantizar la 0.6 (las distribuciones traen la 0.5.x,
# sin notas al pie). Solo la biblioteca de parseo, estática. md4c pide CMake
# >= 3.14 (CMP0077), así que sus option() respetan estas variables normales,
# que solo valen dentro de cli/.
set(BUILD_SHARED_LIBS OFF)
set(BUILD_HTML_LIBRARY OFF)
set(BUILD_MD2HTML_EXECUTABLE OFF)
include(FetchContent)
FetchContent_Declare(
    md4c
    GIT_REPOSITORY https://github.com/mity/md4c.git
    GIT_TAG v0.6.0
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(md4c)
# Sus headers se incluyen como SYSTEM: no es código nuestro y no lleva
# nuestros flags (funciona con CMake 3.21, sin FetchContent ... SYSTEM).
get_target_property(MD4C_INCLUDE_DIRS md4c INTERFACE_INCLUDE_DIRECTORIES)
set_target_properties(md4c PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${MD4C_INCLUDE_DIRS}")

# Lógica de la conversación y del hilo de trabajo: solo depende del núcleo,
# sin FTXUI, para poder probarla sin terminal.
find_package(Threads REQUIRED)
add_library(chatbot_cli_lib STATIC
    src/clipboard.cpp
    src/code_blocks.cpp
    src/command_parser.cpp
    src/conversation.cpp
    src/conversation_export.cpp
    src/conversation_list.cpp
    src/conversation_store.cpp
    src/downloads.cpp
    src/markdown.cpp
    src/models_loader.cpp
    src/provider_settings.cpp
    src/providers.cpp
    src/request_runner.cpp
    src/search_context.cpp
    src/search_settings.cpp
    src/sidebar.cpp
    src/system_prompt.cpp)
target_compile_features(chatbot_cli_lib PUBLIC cxx_std_20)
target_include_directories(chatbot_cli_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
find_package(nlohmann_json 3.9 REQUIRED)
target_link_libraries(chatbot_cli_lib
    PUBLIC chatbot::core Threads::Threads
    PRIVATE nlohmann_json::nlohmann_json md4c)
chatbot_set_warnings(chatbot_cli_lib)

# Dibujo con FTXUI que no necesita la terminal (solo dom y screen), aparte de
# chatbot_cli_lib para que esta siga sin FTXUI. Las pruebas de la vista lo
# usan con ftxui::Screen.
add_library(chatbot_cli_ui STATIC
    src/history_view.cpp
    src/input_style.cpp
    src/list_style.cpp
    src/sidebar_view.cpp
    src/markdown_view.cpp
    src/theme.cpp
    src/title_bar.cpp)
target_compile_features(chatbot_cli_ui PUBLIC cxx_std_20)
target_link_libraries(chatbot_cli_ui PUBLIC chatbot_cli_lib ftxui::dom ftxui::screen)
chatbot_set_warnings(chatbot_cli_ui)

# Ejecutable de la interfaz: el único que usa ftxui::component.
add_executable(chatbot src/main.cpp src/settings_screen.cpp)
target_link_libraries(chatbot PRIVATE chatbot_cli_lib chatbot_cli_ui ftxui::component)
chatbot_set_warnings(chatbot)
