add_executable(smoke smoke.cpp)

target_compile_features(smoke PRIVATE cxx_std_20)
target_link_libraries(smoke PRIVATE chatbot::core)

chatbot_set_warnings(smoke)
chatbot_windows_manifest(smoke)

# Vista previa del render de markdown (sin red ni API).
add_executable(md_preview md_preview.cpp)
target_compile_features(md_preview PRIVATE cxx_std_20)
target_link_libraries(md_preview PRIVATE chatbot_cli_ui)
chatbot_set_warnings(md_preview)
chatbot_windows_manifest(md_preview)
