# chatbot::markdown: markdown → árbol propio con md4c, filtrado del texto y
# bloques de primer nivel con su rango en bytes. Sin FTXUI ni terminal: la
# usan la interfaz (cli/, que dibuja el árbol) y los addons (addons/).

# md4c (parser de markdown en C, MIT). Siempre por FetchContent con versión
# fija: su configuración de CMake no instala un archivo de versión, así que
# find_package no puede garantizar la 0.6 (las distribuciones traen la 0.5.x,
# sin notas al pie). Solo la biblioteca de parseo, estática. md4c pide CMake
# >= 3.14 (CMP0077), así que sus option() respetan estas variables normales,
# que solo valen dentro de markdown/.
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

add_library(chatbot_markdown STATIC
    src/markdown.cpp
    src/source_blocks.cpp)

add_library(chatbot::markdown ALIAS chatbot_markdown)

target_compile_features(chatbot_markdown PUBLIC cxx_std_20)

target_include_directories(chatbot_markdown PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

# md4c no aparece en los headers públicos: PRIVATE.
target_link_libraries(chatbot_markdown PRIVATE md4c)

chatbot_set_warnings(chatbot_markdown)
