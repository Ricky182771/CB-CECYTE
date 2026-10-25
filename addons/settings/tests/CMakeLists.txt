# Pruebas de chatbot::addon_settings, sin red ni systemd: funciones puras y
# carpetas temporales. Usan los helpers de tests/ (temp_dir.hpp...). Corren
# siempre, también con los addons apagados y en Windows.
find_package(nlohmann_json 3.9 REQUIRED)
find_package(Threads REQUIRED)

add_executable(chatbot_addon_settings_tests
    test_bot_model.cpp
    test_invited.cpp
    test_limits.cpp
    test_paths.cpp
    test_registry.cpp
    test_systemd_unit.cpp
    test_telegram_section.cpp
    test_time_zone.cpp
    test_token.cpp)
target_compile_features(chatbot_addon_settings_tests PRIVATE cxx_std_20)
target_include_directories(chatbot_addon_settings_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests)
target_link_libraries(chatbot_addon_settings_tests PRIVATE
    chatbot::addon_settings Catch2::Catch2WithMain nlohmann_json::nlohmann_json
    Threads::Threads)
chatbot_set_warnings(chatbot_addon_settings_tests)
chatbot_windows_manifest(chatbot_addon_settings_tests)
add_test(NAME chatbot_addon_settings_tests COMMAND chatbot_addon_settings_tests)
