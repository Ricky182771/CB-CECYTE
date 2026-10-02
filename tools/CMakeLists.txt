add_executable(smoke smoke.cpp)

target_compile_features(smoke PRIVATE cxx_std_20)
target_link_libraries(smoke PRIVATE chatbot::core)

chatbot_set_warnings(smoke)
