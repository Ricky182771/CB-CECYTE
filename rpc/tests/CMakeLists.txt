# Pruebas de chatbot-rpc, sin red: las puras, el Dispatcher con la Session de
# session_fakes.hpp y, solo en POSIX, el ejecutable real con pipes. Usan los
# helpers de tests/.
add_executable(chatbot_rpc_tests test_rpc_pure.cpp test_rpc_dispatcher.cpp)
if(NOT WIN32)
    target_sources(chatbot_rpc_tests PRIVATE test_rpc_process.cpp)
    add_dependencies(chatbot_rpc_tests chatbot-rpc)
    target_compile_definitions(chatbot_rpc_tests PRIVATE
        CHATBOT_RPC_EXECUTABLE="$<TARGET_FILE:chatbot-rpc>")
endif()
target_compile_features(chatbot_rpc_tests PRIVATE cxx_std_20)
target_include_directories(chatbot_rpc_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests)
target_link_libraries(chatbot_rpc_tests PRIVATE chatbot_rpc_lib Catch2::Catch2WithMain)
chatbot_set_warnings(chatbot_rpc_tests)
chatbot_windows_manifest(chatbot_rpc_tests)
add_test(NAME chatbot_rpc_tests COMMAND chatbot_rpc_tests)
