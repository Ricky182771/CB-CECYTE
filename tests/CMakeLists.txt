# Catch2 v3 es obligatoria (sección 4). Se usa la del sistema si existe una
# v3; si no, FetchContent con versión fija (nunca master/main).
find_package(Catch2 3 QUIET COMPONENTS Catch2WithMain)
if(NOT Catch2_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.16.0
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(Catch2)
endif()

find_package(nlohmann_json 3.9 REQUIRED)

add_executable(chatbot_tests
    test_block_actions.cpp
    test_cancel.cpp
    test_clipboard.cpp
    test_code_blocks.cpp
    test_command_menu.cpp
    test_command_parser.cpp
    test_commands.cpp
    test_config.cpp
    test_conversation.cpp
    test_conversation_export.cpp
    test_conversation_list.cpp
    test_conversation_store.cpp
    test_debug_sse.cpp
    test_downloads.cpp
    test_glyphs.cpp
    test_provider_core.cpp
    test_error_mapping.cpp
    test_history.cpp
    test_input_edit.cpp
    test_key_log.cpp
    test_markdown.cpp
    test_models_loader.cpp
    test_paste.cpp
    test_provider_settings.cpp
    test_platform.cpp
    test_published_date.cpp
    test_redirect.cpp
    test_response.cpp
    test_retries.cpp
    test_request_body.cpp
    test_request_runner.cpp
    test_search_config.cpp
    test_search_context.cpp
    test_search_settings.cpp
    test_sidebar.cpp
    test_sse.cpp
    test_streaming.cpp
    test_system_prompt.cpp
    test_system_prompt_invariants.cpp
    test_tavily_search.cpp
    test_terminal_reply.cpp
    test_web_search.cpp)

target_compile_features(chatbot_tests PRIVATE cxx_std_20)
target_include_directories(chatbot_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/core/src)
target_link_libraries(chatbot_tests PRIVATE
    chatbot::core chatbot_cli_lib Catch2::Catch2WithMain nlohmann_json::nlohmann_json)
# Archivos de tests/data (solo lectura).
# cli/src: test_glyphs.cpp revisa que ningún símbolo se escriba fuera de la tabla.
# tools: test_glyphs.cpp compara la lista de check_glyphs.ps1 con la de glyphs.cpp.
target_compile_definitions(chatbot_tests PRIVATE
    CHATBOT_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
    CHATBOT_CLI_SOURCE_DIR="${PROJECT_SOURCE_DIR}/cli/src"
    CHATBOT_TOOLS_DIR="${PROJECT_SOURCE_DIR}/tools")

chatbot_set_warnings(chatbot_tests)
chatbot_windows_manifest(chatbot_tests)

add_test(NAME chatbot_tests COMMAND chatbot_tests)

# Pruebas de la vista de markdown: dibujan con ftxui::Screen (sin terminal).
# Van aparte para que chatbot_tests no enlace FTXUI.
add_executable(chatbot_ui_tests
    ${PROJECT_SOURCE_DIR}/cli/src/settings_screen.cpp
    ${PROJECT_SOURCE_DIR}/cli/src/text_area.cpp
    test_settings_screen.cpp
    test_command_menu_view.cpp
    test_history_view.cpp
    test_input_box.cpp
    test_input_style.cpp
    test_sidebar_view.cpp
    test_status_line.cpp
    test_symbols_view.cpp
    test_text_area.cpp
    test_text_edit.cpp
    test_markdown_view.cpp
    test_theme.cpp
    test_title_bar.cpp)
target_compile_features(chatbot_ui_tests PRIVATE cxx_std_20)
target_link_libraries(chatbot_ui_tests PRIVATE chatbot_cli_ui ftxui::component Catch2::Catch2WithMain)
# Archivos de tests/data (solo lectura).
target_compile_definitions(chatbot_ui_tests PRIVATE
    CHATBOT_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
chatbot_set_warnings(chatbot_ui_tests)
chatbot_windows_manifest(chatbot_ui_tests)
add_test(NAME chatbot_ui_tests COMMAND chatbot_ui_tests)

# Pegado entre corchetes por una pseudoterminal a chatbot_paste_probe, un App
# mínimo con BracketedPaste como main.cpp: la regresión del parche de FTXUI
# (cmake/patches/, 200 KB) y la salida de emergencia sin 201~. Solo POSIX: el
# parche solo cambia la lectura de POSIX y la prueba usa posix_openpt y fork.
if(NOT WIN32)
    add_executable(chatbot_paste_probe paste_probe.cpp)
    target_compile_features(chatbot_paste_probe PRIVATE cxx_std_20)
    target_link_libraries(chatbot_paste_probe PRIVATE chatbot_cli_lib ftxui::component)
    chatbot_set_warnings(chatbot_paste_probe)
    target_sources(chatbot_ui_tests PRIVATE test_paste_pty.cpp)
    target_compile_definitions(chatbot_ui_tests PRIVATE
        CHATBOT_PASTE_PROBE="$<TARGET_FILE:chatbot_paste_probe>")
    add_dependencies(chatbot_ui_tests chatbot_paste_probe)
endif()

# Modo portable con el ejecutable real: copia chatbot junto a un
# chatbot.portable y lo arranca sin key ni terminal (portable_startup.cmake).
add_test(NAME chatbot_portable_writable
    COMMAND ${CMAKE_COMMAND} -DCHATBOT=$<TARGET_FILE:chatbot>
        -DWORK=${CMAKE_CURRENT_BINARY_DIR}/portable_writable -DMODE=writable
        -P ${CMAKE_CURRENT_SOURCE_DIR}/portable_startup.cmake)
# Carpeta del programa de solo lectura: solo POSIX (en Windows manda la ACL).
if(NOT WIN32)
    add_test(NAME chatbot_portable_readonly
        COMMAND ${CMAKE_COMMAND} -DCHATBOT=$<TARGET_FILE:chatbot>
            -DWORK=${CMAKE_CURRENT_BINARY_DIR}/portable_readonly -DMODE=readonly
            -P ${CMAKE_CURRENT_SOURCE_DIR}/portable_startup.cmake)
    set_tests_properties(chatbot_portable_readonly PROPERTIES SKIP_REGULAR_EXPRESSION "SKIP:")
endif()
