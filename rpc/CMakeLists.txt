# chatbot-rpc: la app por JSON-RPC 2.0 sobre stdin/stdout (PROTOCOL.md), para
# otras interfaces (la GUI de Flutter). Sin FTXUI ni addons: enlaza
# chatbot_cli_lib, la misma lógica de la TUI (Session). chatbot_rpc_lib tiene
# todo menos main.cpp, para probarlo sin procesos.
find_package(nlohmann_json 3.9 REQUIRED)

# stdout solo del protocolo y, en Windows, stdio en modo binario.
if(WIN32)
    set(CHATBOT_RPC_STDIO_SOURCE src/stdio_windows.cpp)
else()
    set(CHATBOT_RPC_STDIO_SOURCE src/stdio_posix.cpp)
endif()

add_library(chatbot_rpc_lib STATIC
    src/debug_log.cpp
    src/dispatcher.cpp
    src/entry_diff.cpp
    src/line_reader.cpp
    src/protocol.cpp
    ${CHATBOT_RPC_STDIO_SOURCE})
target_compile_features(chatbot_rpc_lib PUBLIC cxx_std_20)
target_include_directories(chatbot_rpc_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(chatbot_rpc_lib PUBLIC chatbot_cli_lib nlohmann_json::nlohmann_json)
chatbot_set_warnings(chatbot_rpc_lib)

add_executable(chatbot-rpc src/main.cpp)
target_link_libraries(chatbot-rpc PRIVATE chatbot_rpc_lib)
# server.version de initialize.
target_compile_definitions(chatbot-rpc PRIVATE CHATBOT_VERSION="${PROJECT_VERSION}")
chatbot_set_warnings(chatbot-rpc)
chatbot_windows_manifest(chatbot-rpc)

if(CHATBOT_BUILD_TESTS)
    add_subdirectory(tests)
endif()
