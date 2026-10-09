# AGENTS.md — Reglas del proyecto Chatbot (terminal, núcleo en C++)

> Lee este archivo completo antes de escribir cualquier código. Si una instrucción del usuario contradice algo de aquí, gana el usuario, pero avísale de la contradicción antes de continuar.

## 1. Qué es el proyecto

Un chatbot tipo asistente general que corre en la terminal (pantalla completa, estilo OpenCode). Proyecto escolar en equipo, de alcance chico.

- **Núcleo (`core/`)**: biblioteca C++ con cliente HTTP, historial, configuración y manejo de errores. No sabe nada de terminales ni de interfaces.
- **Interfaz (`cli/`)**: ejecutable con FTXUI. **No se toca hasta el hito 3.**
- **Servidor**: API de NVIDIA NIM (plan gratuito), compatible con OpenAI: `POST {base_url}/chat/completions`.

## 2. Cómo debes comportarte

1. **Nunca asumas.** Si algo no está especificado aquí o es ambiguo, pregunta antes de decidir. Esto incluye nombres de modelos, formatos de archivo, dependencias y cambios de diseño.
2. **Pregunta antes de decidir por tu cuenta** cualquier cosa que cambie la arquitectura, agregue una dependencia o salga del alcance del hito actual.
3. **No declares nada terminado sin verificarlo.** "Debería funcionar" no cuenta. Compila, corre las pruebas y los sanitizadores, y muestra la salida real de los comandos.
4. **Toma el tiempo que haga falta.** Es preferible algo lento que pase todo a algo rápido que falle.
5. **Cambios pequeños y enfocados.** Un hito a la vez. No refactorices código ajeno a la tarea.
6. **No inventes APIs.** Si no estás seguro de una función de libcurl, nlohmann/json o Catch2, consulta la documentación oficial o pregunta.
7. **No hagas commit ni push** a menos que se te pida.
8. **No apagues warnings, sanitizadores ni pruebas para que algo pase.** Si fallan, arréglalo o explica por qué no puedes.
9. **Reporta con honestidad:** qué hiciste, qué no, qué falló y qué dudas quedan. Si te atoras, dilo y pregunta; no hagas soluciones alternativas en silencio.

## 3. Alcance por hitos

| Hito | Contenido | Estado |
|------|-----------|--------|
| 1 | Andamiaje + núcleo con petición simple (sin streaming) + errores tipados + configuración | Hecho |
| 2 | Streaming SSE + parser + pruebas del parser | Hecho |
| — | Correcciones del núcleo (excepciones, `content: null`, timeout de streaming, errores permanentes, límite de timeout) | Hecho |
| 3 | Interfaz FTXUI mínima (historial arriba, caja de entrada abajo) | Hecho |
| 4 | Cancelación, scroll del historial, recorte de historial largo, prueba automatizada del hilo de trabajo (ver nota) | Hecho |
| 5 | Persistencia de conversaciones y render de markdown | Hecho |
| — | Mantenimiento: preset release, CI, pulido de interfaz | Hecho |
| — | Pantalla de configuración, sección "Proveedor de IA" (núcleo, lógica, interfaz y documentación) | Hecho |
| — | Temas de color (`Theme`, `Palette`), selección y foco visibles, sección "Colores y Accesibilidad" | Hecho |
| — | Instrucciones del sistema configurables (`system_prompt` en `config.json`, sección "Instrucciones del sistema") | Hecho |
| — | Búsqueda web fase 1: `/buscar` con Tavily, resultados como datos en el mensaje `User`, bloque "Fuentes", guardado de `"search"`, categoría "Búsqueda web" | Hecho |
| — | Copiar, guardar y exportar: `/copiar`, `/guardar`, `/exportar`, numeración visible de los bloques de código, portapapeles (programas u OSC 52), carpeta de descargas (`CHAT_DOWNLOAD_DIR`); latencia de cancelación < 2 s en todos los presets | Hecho |
| — | Botones `[Copiar]` y `[Guardar]` fijos en cada bloque de código y `[Exportar]` en la barra de título (`block_actions`, cajas de los bloques en `md::render`, `HistoryView::hit_test`); aviso de bloque incompleto; permisos de las descargas según la umask | Hecho |
| W1 | Windows, capa de plataforma: manifiesto UTF-8, `chatbot/platform.h`, rutas de Windows, credenciales sin permisos POSIX, descargas con Win32, portapapeles nativo, certificados de Windows, consola en UTF-8, CI de Windows | En revisión |
| W2, W3 | Windows: QuickEdit, glifos de respaldo y ratón en conhost (W2); ZIP con Windows Terminal portable, DPAPI y compilación cruzada desde Fedora (W3) | Fuera de alcance |

**Nota para el hito 4: prueba con hilos** (sin loop de FTXUI). Implementada con `RequestRunner` (`cli/src/request_runner.*`) y `tests/test_request_runner.cpp`; el transporte que se bloquea quedó aparte, en `tests/blocking_transport.hpp`:

- Extraer el hilo de trabajo de `cli/src/main.cpp` a una clase sin FTXUI que reciba una función para pasar tareas al hilo de la interfaz. En producción esa función hace `screen.Post` + `PostEvent(Event::Custom)`; en las pruebas, mete las tareas en una cola que la prueba vacía.
- Agregar a `FakeTransport` un bloqueo controlable para detener el flujo a la mitad.
- Correr la prueba con el preset `tsan`.

Todo lo de hitos posteriores está **fuera de alcance** del hito actual. No lo adelantes.

## 4. Stack

- **Lenguaje:** C++20. No usar funciones de C++23 (por ejemplo, `std::expected`).
- **Build:** CMake 3.21 o superior, con presets. Generador Ninja.
- **Compiladores:** GCC y Clang deben compilar sin warnings.
- **Windows:** MSYS2 UCRT64 con GCC, con los mismos flags y `-Werror`; sin MSVC. Destino: Windows 10 IoT LTSC 21H2 (19044) con conhost y Windows 11 con Windows Terminal (el manifiesto pide Windows 10 1903 o posterior). Las bibliotecas del sistema (kernel32, user32, shell32, ole32) no cuentan como dependencias. Solo con `WIN32` y solo en nuestros targets: `_WIN32_WINNT=0x0A00`, `WIN32_LEAN_AND_MEAN` y `NOMINMAX` (`chatbot_windows_definitions`, que llama `chatbot_set_warnings`). `asan` y `tsan` son solo de Linux.
- **Dependencias permitidas:** libcurl, nlohmann/json ≥ 3.9 (3.x; la 3.9 trae `ordered_json`, que se usa para escribir las conversaciones en el orden del esquema), Catch2 v3 (solo pruebas), FTXUI ≥ 7.0.2 (solo `cli/`; fijada a v7.0.3 en FetchContent), md4c v0.6.0 (solo `cli/`, render de markdown; siempre por FetchContent con `GIT_TAG v0.6.0`: su CMake no instala archivo de versión y las distribuciones traen la 0.5.x, sin notas al pie; solo la biblioteca de parseo, estática, con sus headers como `SYSTEM`).
- Resolver dependencias con `find_package`; si no están instaladas, usar `FetchContent` con versión fija (nunca `master`/`main`).
- **Ninguna otra dependencia sin preguntar.**

## 5. Estructura de directorios

```
chatbot/
├── AGENTS.md
├── CMakeLists.txt
├── CMakePresets.json
├── .gitignore
├── .gitattributes           # saltos de línea: LF; *.ans sin conversión; *.cmd/*.bat CRLF
├── .github/workflows/ci.yml  # CI: GCC (dev, asan, tsan, release), Clang (dev, release)
│                            # y Windows con MSYS2 UCRT64 (dev, release)
├── cmake/windows/           # chatbot.manifest (UTF-8) y manifest.rc: chatbot_windows_manifest
├── core/
│   ├── CMakeLists.txt
│   ├── include/chatbot/     # headers públicos (web_search.h, tavily_search.h: búsqueda web;
│   │                        # utf8.h: decodificador UTF-8 mínimo que también usa cli/;
│   │                        # platform.h: capa de plataforma; platform_windows.h:
│   │                        # UTF-8 <-> UTF-16 y FormatMessageW, solo para .cpp de Windows)
│   └── src/                 # platform_posix.cpp o platform_windows.cpp (CMake elige uno)
│                            # y platform_common.cpp (funciones puras)
├── tools/                   # smoke.cpp: programa desechable para probar el núcleo;
│                            # md_preview.cpp: vista previa del render de markdown
├── cli/
│   ├── CMakeLists.txt       # FTXUI y md4c, chatbot_cli_lib, chatbot_cli_ui y ejecutable chatbot
│   └── src/
│       ├── conversation.h/.cpp        # lógica de la conversación (sin FTXUI ni hilos)
│       ├── conversation_store.h/.cpp  # archivos de conversación (sin FTXUI)
│       ├── conversation_list.h/.cpp   # lógica de la lista de conversaciones (sin FTXUI)
│       ├── sidebar.h/.cpp             # barra de conversaciones: grupos por fecha, navegación (sin FTXUI)
│       ├── request_runner.h/.cpp      # hilo de trabajo de la petición (sin FTXUI)
│       ├── providers.h/.cpp           # tabla de proveedores compatibles con OpenAI (sin FTXUI)
│       ├── provider_settings.h/.cpp   # lógica del formulario "Proveedor de IA" (sin FTXUI)
│       ├── models_loader.h/.cpp       # hilo de trabajo de GET /models (sin FTXUI)
│       ├── system_prompt.h/.cpp       # instrucciones de sistema: predeterminadas, validación y resolución (sin FTXUI)
│       ├── command_parser.h/.cpp      # reconoce /buscar, /copiar, /guardar y /exportar (sin FTXUI)
│       ├── code_blocks.h/.cpp         # numeración de los bloques de código: collect_code_blocks, CodeBlockIndex (sin FTXUI)
│       ├── clipboard.h/.cpp           # portapapeles: selección del método, OSC 52, to_crlf (sin FTXUI)
│       ├── clipboard_posix.cpp        # lanzador de programas (posix_spawnp) y PATH; solo POSIX
│       ├── clipboard_windows.cpp      # portapapeles de Windows (Win32, CF_UNICODETEXT); solo Windows
│       ├── downloads.h/.cpp           # carpeta de descargas, nombres, extensiones, escritura sin sobrescribir (sin FTXUI)
│       ├── conversation_export.h/.cpp # /exportar: conversación → Markdown (sin FTXUI)
│       ├── block_actions.h/.cpp       # copiar, guardar y exportar → ActionResult, para comandos y botones (sin FTXUI)
│       ├── search_context.h/.cpp      # nonce del bloque, fechas y avisos de la búsqueda (sin FTXUI)
│       ├── search_settings.h/.cpp     # lógica del formulario "Búsqueda web" (sin FTXUI)
│       ├── settings_screen.h/.cpp     # pantalla de configuración (ejecutable chatbot, ftxui::component)
│       ├── markdown.h/.cpp            # markdown → árbol propio con md4c, y filtrado del texto (sin FTXUI)
│       ├── markdown_view.h/.cpp       # árbol de markdown → ftxui::Element (chatbot_cli_ui)
│       ├── history_view.h/.cpp        # entradas de la conversación, con caché (chatbot_cli_ui)
│       ├── input_style.h/.cpp         # estilo de la caja de entrada, sin invertido (chatbot_cli_ui)
│       ├── theme.h/.cpp               # tabla de temas, Palette, contraste WCAG (chatbot_cli_ui)
│       ├── list_style.h/.cpp          # cursor, elegido, etiquetas y botones con foco (chatbot_cli_ui)
│       ├── sidebar_view.h/.cpp        # dibujo de la barra de conversaciones (chatbot_cli_ui)
│       ├── title_bar.h/.cpp           # barra de título con [Exportar] (chatbot_cli_ui)
│       ├── status_line.h/.cpp         # línea de estado: indicadores reservados, avisos recortados con … (chatbot_cli_ui)
│       └── main.cpp             # interfaz FTXUI (ejecutable chatbot)
└── tests/                   # chatbot_tests (sin FTXUI) y chatbot_ui_tests (vista)
    │                        # temp_dir.hpp, env_guard.hpp y posix_permissions.hpp: helpers
    │                        # portables; test_platform.cpp: capa de plataforma
    └── data/                # markdown_muestra.md: muestra con todos los elementos
```

- Biblioteca estática `chatbot_core`, namespace `chatbot`.
- `core/` no puede depender de FTXUI ni leer/escribir en la terminal (salvo `tools/`).
- `chatbot_cli_lib` (biblioteca estática de `cli/`) solo depende de `chatbot::core` (y, en privado, de nlohmann/json y md4c), sin FTXUI, para poder probarla.
- `chatbot_cli_ui` (biblioteca estática de `cli/`) dibuja sin terminal: enlaza `chatbot_cli_lib`, `ftxui::dom` y `ftxui::screen`. La usan el ejecutable `chatbot`, `tools/md_preview` y `chatbot_ui_tests`. El ejecutable `chatbot` y, como excepción autorizada, `chatbot_ui_tests` enlazan `ftxui::component`: las pruebas compilan `SettingsScreen` para enviarle eventos reales y dibujar con `Screen`, sin abrir una terminal.
- Conversaciones guardadas: una barra lateral a la izquierda (lógica en `Sidebar`, que reusa `ConversationList`; dibujo en `sidebar_view`). Ctrl+B la muestra u oculta, Ctrl+O le da el foco y Ctrl+N empieza una conversación nueva. Se muestra al arrancar con 100 columnas o más y su borde derecho es el divisor de `ResizableSplit`. Reemplaza a la lista de pantalla completa. Sus dos primeras filas son fijas: `+ Nueva` y `⚙ Configuración`.
- Configuración: F2 o la fila `⚙ Configuración` abren `SettingsScreen` en lugar de la conversación (no con una respuesta en curso). La tabla de proveedores está solo en `providers.cpp`; el estado del formulario, en `ProviderSettings` (campos bloqueados por `CHAT_*`, key enmascarada con `mask_key`: nunca más de 4 caracteres); `GET /models` corre en `ModelsLoader` (un hilo y un `CancelToken` por petición; cerrar la pantalla o pedir otra lista cancela la anterior sin esperarla, y el destructor une los hilos). Guardar escribe `credentials.json` y `config.json`, vuelve a cargar la configuración y reconstruye `ChatClient` y `RequestRunner`. Si al arrancar solo falta la key o el modelo y hay terminal, la app abre la configuración (`ConfigOptions::allow_missing_key_and_model`) y la caja no envía; sin terminal (stdin o stdout redirigidos) sale con el error, como antes.
- Colores: solo desde el `Theme` (tabla de datos en `theme.cpp`; agregar un tema = agregar una entrada, las pruebas revisan su contraste). Ningún `Color::`, `dim`, `inverted` ni `bgcolor` fuera de `theme.cpp`: las vistas reciben una `Palette` (tema + fondo "Del tema"/"Transparente") y usan `ink(&Theme::campo)`, `base()` (fondo y texto en la raíz), `selection()`, `highlight()`, `inside_border()` y `vscroll()`. Tema "De la terminal": paleta de 16 colores, `dim` e `inverted`, igual que antes de los temas. Selección (`list_style`): cursor = fila completa con `selection()` y solo con el foco, sin `"> "`; elegido = `"● "` en `heading_accent` y negritas. La caché de `HistoryView` incluye `Palette::key()`.
- **Capa de plataforma** (`chatbot/platform.h`): lo único que cambia entre POSIX y Windows. CMake compila `platform_posix.cpp` o `platform_windows.cpp` (y `clipboard_posix.cpp` o `clipboard_windows.cpp` en `cli/`); nada de `#ifdef` en la lógica (las pruebas sí pueden usar `#ifndef _WIN32` para lo que solo existe en POSIX). `<windows.h>` solo en los `.cpp` de Windows. `std::string` es UTF-8 en todas las plataformas: en Windows lo garantiza el manifiesto (`activeCodePage` UTF-8 en todos los ejecutables, `chatbot_windows_manifest`) y las llamadas a Win32 usan la versión W con `utf8_to_utf16`/`utf16_to_utf8` (`platform_windows.h`). Funciones: `current_os()` (las funciones puras que cambian según el sistema reciben un `Os`, para probar las reglas de Windows también en Linux), `local_time` (`localtime_r`/`localtime_s`), `stdio_is_terminal`/`stdout_is_terminal` (`isatty`/`GetConsoleMode`, nunca `_isatty`, que da verdadero con `NUL`), `known_folder` (`SHGetKnownFolderPath`; en POSIX, nullopt), `join_windows_path`, `check_secret_file`, `create_private_directory`, `write_file_atomic` (la única; `FilePrivacy::Private` = 0600, `KeepExisting` = los permisos que tenía o 0644; `FolderPrivacy::Private` = 0700 siempre, `PrivateIfNew` = 0700 al crearla; en Windows, `.tmp` + `FlushFileBuffers` + `MoveFileExW(MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)` con hasta 5 reintentos de 50 ms si da acceso denegado o archivo en uso), `create_new_file`/`create_directory` (descargas: `O_EXCL | O_NOFOLLOW` y 0644/0755 menos la umask, o `CREATE_NEW`/`CreateDirectoryW`) y `ConsoleUtf8Scope` (en Windows pone `CP_UTF8` en la consola y restaura las páginas de códigos al salir; `main` la declara primero). Errores en español; en Windows con el texto de `FormatMessageW`.
- **Ctrl+C en Windows:** `ConsoleInputScope` (`interactive_console_input_mode`, pura) quita `ENABLE_PROCESSED_INPUT` del modo de entrada mientras corre `main`, porque FTXUI 7.0.3 lo deja puesto y la consola convierte Ctrl+C en `CTRL_C_EVENT` → `SIGINT` → el proceso muere dentro de `Loop()` sin destruir el runner ni restaurar la página de códigos; sin el bit, llega como 0x03 (`Event::CtrlC`) y sale por `screen.Exit()`. Limitación: Ctrl+Break (`CTRL_BREAK_EVENT`) y cerrar la ventana siguen terminando el proceso.
- `chatbot/utf8.h` (`utf8::next_code_point`, `utf8::is_valid`, `utf8::kReplacement`) es público y mínimo para que `cli/` (`validate_system_prompt`) use el mismo decodificador que el núcleo; `chatbot_cli_lib` nunca incluye `core/src`.
- Búsqueda web (`/buscar`): `parse_command` reconoce el comando; `RequestRunner::start_with_search` busca y llama al modelo en el mismo hilo y con el mismo `CancelToken`; `Conversation::attach_search` reemplaza el content del último `User` por el bloque y guarda la búsqueda con el par; `HistoryView` dibuja la entrada `Sources` ("Fuentes", `[n] título — fecha (url)`, sin fecha si no hay, armada a mano como `md::Document`, sin parsear markdown). El proveedor (`std::shared_ptr<SearchProvider>`) se reconstruye al guardar la key.
- Copiar, guardar y exportar (`/copiar [n]`, `/guardar [n [nombre]]`, `/exportar`): `parse_command` da un tipo por comando y uno por error de uso (`CopyUsage`, `SaveUsage`, `ExportUsage`). No van al modelo ni al historial; el resultado es un `Notice` (los errores de uso y "Espera a que termine la respuesta", en la línea de estado; los errores de escritura o de copiado, una entrada de error). Los bloques se numeran desde 1 en toda la conversación con `collect_code_blocks` (respuestas del asistente, también la que sigue llegando, en el orden en que las dibuja `md::render`); `main.cpp` usa `CodeBlockIndex` (la misma numeración, con caché por entrada) y pasa a `HistoryView::render` el número inicial de cada entrada, que es parte de la llave de su caché; el marco dice `#3 · cpp`. `md::render` sin número (`first_code = 0`, como en `md_preview`) dibuja el título de antes.
  - **Acciones** (`block_actions.h`): `copy_block`, `save_block`, `run_block_action` (número de bloque + `BlockAction`) y `export_conversation` regresan `ActionResult { message, error }` sin tocar la conversación, el scroll ni la caja; el portapapeles llega inyectado (`ClipboardAccess`; `real_clipboard()` en producción). Los comandos muestran el resultado en el historial con los textos largos (`Wording::Long`); los botones, en la línea de estado con `Wording::Short` (`run_block_action`): "#1 copiado con wl-copy", "#1 copiado con OSC 52 · si no pega, usa [Guardar]", "⚠ #1 incompleto (aún no termina | se canceló | se cortó) · copiado con …", "#1 guardado en ~/…", "No se pudo copiar #1 · usa [Guardar]". La línea de estado (`status_line`) reserva el ancho de los indicadores ("Pensando…", "Buscando en la web…", "↓ Hay más abajo"; si no caben juntos se quitan desde el último, nunca el primero) y los avisos usan lo que sobra, recortados con "…". `CodeBlock::complete` es falso si la respuesta se canceló, se cortó por un error o sigue llegando (`IncompleteReason`), y `incomplete_warning` agrega "Ojo: el bloque está incompleto (…)" a los avisos.
  - **Botones de los bloques**: `md::render` recibe `std::vector<ftxui::Box>* code_boxes` (un elemento por bloque, en el orden de `code_blocks_of`, con `reflect` en cada marco; con `nullptr` la salida es idéntica); `HistoryView::Cached` guarda `CodeFrame { number, box, title_width }` en coordenadas de la entrada. `Picture::Render` dibuja `[Copiar] [Guardar]` en cada cuadro después de copiar las celdas visibles: sobre el borde superior si se ve, si no en la primera fila visible del contenido, nunca sobre el borde inferior; alineados a la derecha, terminando una columna antes del borde; `[C] [G]` si no caben después del título con 2 columnas de margen, y nada si tampoco. Tinta `notice` sobre el fondo del bloque, hover con `Palette::select_cell` (invertido con NO_COLOR), celdas con `automerge = false` (`Palette::ink_cell`/`select_cell`: los colores siguen saliendo de `theme.cpp`). Hover y `[✓ Copiado]` (hasta que el puntero sale o pasan `kCopiedFor` = 2 s, con reloj inyectable) son estado de cada cuadro: no cambian `draw_count`. `main.cpp` solo pasa el ratón: `Mouse::Moved` → `set_hover`/`clear_hover`, clic izquierdo sobre un botón → `run_block_action`. Los botones funcionan con una respuesta en curso; los comandos siguen esperando a que termine.
  - **`[Exportar]`** (`title_bar.h`): prioridad de ancho título > botón > ayuda; activo en el color `text` y, sin respuestas, atenuado (`input_placeholder`) y sin acción (`notice` no sirve: en los dos temas es igual a `input_placeholder`); su caja sale con `reflect` para el clic.
  - **Portapapeles** (`clipboard.h`): `clipboard_methods(env, find, os)` (pura) da el orden: OSC 52 primero con `SSH_CONNECTION`/`SSH_TTY`; en POSIX, `termux-clipboard-set` (`TERMUX_VERSION`), `wl-copy` (`WAYLAND_DISPLAY`), `xclip -selection clipboard` y `xsel --clipboard --input` (`DISPLAY`), si están en `PATH`; en Windows, `Native` ("portapapeles de Windows"), sin programas; OSC 52 como último recurso. `copy_to_clipboard` recibe el lanzador, la terminal y el copiador nativo inyectados: las pruebas no lanzan procesos ni tocan el portapapeles. `run_with_input` (solo POSIX, `clipboard_posix.cpp`) usa `posix_spawnp` (nunca un shell), el texto por un pipe a stdin, stdout/stderr a `/dev/null`, SIGPIPE bloqueado al escribir y tope de 2 s (después, `SIGKILL`). `copy_to_native_clipboard` (solo Windows, `clipboard_windows.cpp`): ventana de solo mensajes (`HWND_MESSAGE`) como dueña, `OpenClipboard` con reintentos breves, `EmptyClipboard`, `GlobalAlloc(GMEM_MOVEABLE)` y `SetClipboardData(CF_UNICODETEXT)`, con el texto pasado por `to_crlf` (pura). Sin `clip.exe`: revuelve el UTF-8 según la página de códigos. OSC 52 (`ESC ] 52 ; c ; base64 BEL`, envuelto para tmux con `TMUX`) solo hasta 100 000 bytes; se escribe en `std::cout` desde el hilo de la interfaz, el mismo con el que dibuja FTXUI.
  - **Descargas** (`downloads.h`): carpeta `<descargas>/chatbot` (0755 menos la umask) según la sección 7 (`default_download_dir`). Nombre del usuario validado (`validate_file_name`: sin `/`, `\`, `..`, controles ni `.` inicial, UTF-8 válido, ≤ 100 bytes; y en todas las plataformas, las reglas de Windows: sin `< > : " | ? *`, sin los nombres reservados `CON`, `PRN`, `AUX`, `NUL`, `COM1`–`COM9` y `LPT1`–`LPT9` sin distinguir mayúsculas y también con extensión, y sin terminar en `.` ni en espacio); sin extensión se agrega la del lenguaje (`extension_for`, tabla en `downloads.cpp`); por defecto `bloque-<n>.<ext>`. `write_new_file` usa `create_new_file` (POSIX: `O_CREAT | O_EXCL | O_NOFOLLOW` con 0644 menos la umask, sin `chmod` ni `fchmod`, nunca ejecutable; Windows: `CREATE_NEW`, sin umask), prueba `-2` a `-99` y borra el archivo si la escritura falla. En Windows las rutas se arman con `\` y `display_path` muestra la ruta completa, sin `~` (`display_home`). El bloque se guarda tal cual, con salto de línea final.
  - **Exportar** (`conversation_export.h`): `export_markdown(StoredConversation, fecha)` es pura y solo usa los pares guardados (nunca system prompt, bloques de resultados, avisos ni errores); las URL de "Fuentes" pasan otra vez por `is_web_url` (si no, texto plano). Archivo `conversacion-<id>.md`, o con fecha y hora si no hay id. La prueba compara contra `tests/data/conversacion_exportada.md`.
- Todo texto que viene del modelo, del usuario o de un archivo pasa por `md::sanitize` antes de dibujarse (controles C0 salvo `\n`, ESC, DEL y C1 → U+FFFD; tab → 4 espacios). FTXUI descarta esos caracteres en `text()`, pero sin dejar rastro, y escribe sin filtrar la URL de `hyperlink`: por eso las URL además se codifican con `hyperlink_target`.

## 6. Diseño del núcleo

- **Tipos básicos:** `Role` (`System`, `User`, `Assistant`), `Message { Role role; std::string content; }`, `Config`.
- **Errores:** `ChatError { ErrorKind kind; int http_status; std::string message; std::optional<std::chrono::seconds> retry_after; }`.
  `ErrorKind`: `Config`, `Auth`, `ModelNotFound`, `RateLimited`, `Server`, `Network`, `Timeout`, `BadResponse`, `InvalidRequest`, `Cancelled`, `NoSearchResults`.
  `NoSearchResults` ("búsqueda sin resultados"): la búsqueda web respondió bien pero sin resultados. No es una falla: nunca se reintenta, `RequestRunner` lo manda con el texto `kNoSearchResults` y `Conversation::finish_error` lo detecta por `kind` (no por el texto) y lo muestra como aviso.
- **Resultado:** `Result<T>` mínimo sobre `std::variant<T, ChatError>`. Las excepciones pueden usarse internamente, **pero nunca cruzan la API pública**.
- **Transporte:** interfaz `Transport` que hace el POST (o el GET, con `HttpRequest::method = HttpMethod::Get`, sin cuerpo) y devuelve estado HTTP, cabeceras relevantes (`Retry-After`) y cuerpo, o un error de red. `CurlTransport` es la implementación real; las pruebas usan un `FakeTransport`. Diséñala para poder agregar streaming en el hito 2 sin romper `complete`; si hace falta cambiar algo, pregunta.
- **Cliente:** `ChatClient(Config, std::unique_ptr<Transport>)` con `complete(const std::vector<Message>&, const CancelToken* = nullptr) -> Result<std::string>` y `complete_stream(messages, on_delta, const CancelToken* = nullptr) -> Result<CompletionInfo>`. `CompletionInfo { std::string finish_reason; }` (`chatbot/completion_info.h`): el último `choices[0].finish_reason` no nulo que llegó, o vacío si nunca llegó. `list_models(const CancelToken* = nullptr) -> Result<std::vector<std::string>>`: GET `{base_url}/models`; lee los `data[].id` que sean cadenas (sin `data` arreglo → `BadResponse`), quita el prefijo `models/`, sin duplicados y en orden; mismos errores, reintentos y cancelación que `complete`, y no necesita modelo.
- **Cancelación:** `CancelToken` (`cancel()`, `is_cancelled()`, `wait_for(ms)`; seguro entre hilos, no copiable ni movible; quien lo crea lo mantiene vivo durante la petición). `ChatClient` lo revisa antes y después de cada intento (una cancelación durante un intento gana aunque haya terminado bien), lo pasa al transporte (`HttpRequest::cancel`) y a la espera de reintento; si se cancela, el resultado es `Cancelled` y no se reintenta. En la interfaz, `RequestRunner::cancel()` gana mientras `busy()` sea `true`, aunque la respuesta ya haya terminado en el hilo de trabajo. `CurlTransport` lo revisa desde `CURLOPT_XFERINFOFUNCTION`, que libcurl llama alrededor de una vez por segundo aunque no lleguen datos: la latencia de cancelación es de ~1 s.
- **Reintentos** (dentro de `ChatClient`):
  - Reintentar solo `RateLimited`, `Server`, `Network` y `Timeout`.
  - Máximo 3 reintentos, con espera exponencial desde 1 s. Si hay `Retry-After`, respetarlo.
  - Nunca reintentar `Auth`, `ModelNotFound`, `BadResponse`, `InvalidRequest`, `Config`, `Cancelled` ni `NoSearchResults`.
  - La espera se inyecta (interfaz `Sleeper`: `bool sleep_for(std::chrono::milliseconds, const CancelToken*)`, que devuelve `false` si la cancelación interrumpió la espera) para que las pruebas no esperen de verdad.
- El historial lo administra quien llama; el núcleo no lo guarda.
- **Búsqueda web** (`chatbot/web_search.h`): `SearchResult { title, url, content, published_date }`, `SearchResponse { query, results }`, interfaz `SearchProvider::search(query, const CancelToken*) -> Result<SearchResponse>` (la que usará el tool calling de la fase 2; no depende de la interfaz ni de la conversación). `TavilySearch(api_key, std::unique_ptr<Transport>)` la implementa, sin reintentos automáticos. `is_web_url(url)`: `http://` o `https://` con algo después; el esquema no distingue mayúsculas (`HTTPS://` pasa, RFC 3986 §3.1). La usan `TavilySearch` y el lector de conversaciones (descartan el resultado) y "Fuentes" (dibuja como texto plano, sin enlace, la URL que no pase). `trim_search_response` deja el content limpio (por carácter: controles C0 salvo `\n`, DEL y C1 U+0080–U+009F fuera; `\t` → espacio; bytes inválidos → U+FFFD) y recortado a 1200 bytes por resultado y 6000 en total sin partir UTF-8 (el total se mide antes de neutralizar el cierre); es idempotente y es lo que se guarda. `format_search_context(response, search_date, nonce)` arma el bloque `<resultados id="nonce">…</resultados id="nonce">` con la fecha de la búsqueda (`Fecha de la búsqueda: …`, no "de hoy": al reabrir una conversación es la fecha guardada), la instrucción de tratar los resultados como datos y "Pregunta del usuario"; en título, URL, fecha y consulta `\n` → espacio, y la cadena de cierre se neutraliza en todos los campos.
- **`RequestRunner::start_with_search`:** el historial termina en el `User` del comando; su content se reemplaza por el bloque (nunca dos `User` seguidos ni resultados en un `System`). Un solo canal de error, `on_done`: si la búsqueda falla, se cancela o no trae resultados (`NoSearchResults`, con el texto `kNoSearchResults`), no se llama al modelo. `on_search_done` solo llega con resultados, antes del primer delta. El recorte, el reintento por contexto y el aviso final son los mismos de `start()` (función privada `complete`).
- **Botones de la interfaz (nota):** `HistoryView::hit_test(x, y)` responde con los botones del **cuadro anterior** (`hits_` se vacía en `render()` y lo llena `Picture::Render`): un clic llega después de que el usuario vio ese cuadro. No hay que llamar a `render()` entre el evento y `hit_test`.
- **Recorte del historial:** `trim_history(messages, limit_bytes) -> TrimResult{messages, dropped}` (función pura, `chatbot/history.h`). Nunca quita los mensajes `System` iniciales ni el último; quita los más viejos en pares usuario+asistente hasta caber en el límite (bytes UTF-8 de los `content`). `0` = sin límite.

## 7. Configuración

Precedencia: **variables de entorno > archivo de configuración > valores por defecto.**

| Dato | Variable | Archivo | Por defecto |
|------|----------|---------|-------------|
| Proveedor (id del proveedor o `custom`) | — | `provider` | ninguno |
| API key | `CHAT_API_KEY` | **nunca en `config.json`**; en `credentials.json`, por proveedor | ninguno (error `Config` si falta, salvo en host local) |
| URL base | `CHAT_BASE_URL` | `base_url` | `https://integrate.api.nvidia.com/v1` |
| Modelo | `CHAT_MODEL` | `model` | **ninguno** (error `Config` con mensaje claro si falta) |
| Timeout (s): total en `complete`, por inactividad en `complete_stream` | `CHAT_TIMEOUT` | `timeout_seconds` | 120 |
| Límite del historial que se envía, en bytes UTF-8 de los `content` (aproximadamente caracteres); `0` = sin límite | `CHAT_HISTORY_LIMIT` | `history_limit` | 32000 |
| Instrucciones de sistema (`""` = sin mensaje de sistema) | — | `system_prompt` | `kDefaultSystemPrompt` (`cli/src/system_prompt.h`) |
| Archivo de volcado de depuración | `CHAT_DEBUG_SSE` | **nunca** | ninguno (sin volcado) |
| Carpeta de conversaciones guardadas (la lee `cli/`, no `Config`) | `CHAT_DATA_DIR` | **nunca** | `$XDG_DATA_HOME/chatbot/conversations` si `XDG_DATA_HOME` es ruta absoluta; si no, `~/.local/share/chatbot/conversations`. Windows: `%LOCALAPPDATA%\chatbot\conversations` (`FOLDERID_LocalAppData`; si falla, la variable `LOCALAPPDATA`) |
| Carpeta de `/guardar` y `/exportar` (la lee `cli/`, no `Config`; se le agrega `chatbot/`) | `CHAT_DOWNLOAD_DIR` | **nunca** | `XDG_DOWNLOAD_DIR` de `$XDG_CONFIG_HOME/user-dirs.dirs` (o `~/.config/user-dirs.dirs`, con `$HOME` expandido) si existe; en Termux, `~/storage/downloads` si existe; `~/Descargas` o `~/Downloads`, la que exista; si no, `$HOME`. Windows: la carpeta Descargas (`FOLDERID_Downloads`); si falla, `%USERPROFILE%` |

- Archivo: `$XDG_CONFIG_HOME/chatbot/config.json`, o `~/.config/chatbot/config.json` si no existe esa variable. En Windows, `%APPDATA%\chatbot\config.json` (`FOLDERID_RoamingAppData`; si falla, la variable `APPDATA`). Que el archivo no exista no es un error.
- **Windows:** las variables `CHAT_*` siguen teniendo prioridad, pero se ignoran `HOME` y `XDG_*` (MSYS2 define `HOME` y la configuración quedaría partida según la shell). Los resolvedores son funciones puras que reciben los valores (`build_windows_config_path`, `resolve_windows_data_dir`, `resolve_windows_download_dir`) y se prueban también en Linux; solo `known_folder` lee la carpeta de Windows. Los avisos de carpeta faltante (`missing_config_dir_message`, `no_download_dir_message`) dicen qué faltó en cada sistema.
- **Sin modelo por defecto a propósito:** los modelos de NIM se retiran con el tiempo y un nombre fijo en el código acabaría roto.
- **La key:** primero `CHAT_API_KEY`; si no está, `credentials.json` (junto a `config.json`) con la clave del `provider` del archivo de configuración; si no, ninguna. Formato: `{ "version": 1, "keys": { "nvidia": "...", "custom:https://mi-servidor/v1": "..." } }` (los extremos propios van por su `base_url` efectiva, sin `/` final). Carpeta en 0700 y archivo en 0600, con escritura atómica; si el grupo u otros tienen algún permiso, no se lee y se devuelve error `Config` ("corre chmod 600 <ruta>"). En Windows no hay revisión de permisos (`check_secret_file`): `stat` de MinGW reporta bits de grupo y otros y el archivo siempre saldría "con permisos demasiado abiertos"; lo protege la ACL del perfil del usuario (`%APPDATA%`) y el mensaje "chmod 600" nunca aparece. DPAPI queda para W3. **Por qué cambió** (antes la key solo venía del entorno): la pantalla de configuración necesita guardar la key sin que el usuario edite su shell; un archivo aparte, nunca `config.json`, la deja fuera de lo que se comparte o versiona.
- `save_config_file` escribe `provider`, `base_url` (siempre) y `model` en `config.json`, conserva las demás claves y su orden, y escribe de forma atómica; nunca escribe la key. `load_config_file_values` los lee tal cual (sin entorno) para la pantalla de configuración.
- **Apariencia:** `"appearance": {"theme": "<id>", "background": "theme" | "terminal"}` en `config.json` (sin variable de entorno). `load_appearance_values`/`save_appearance` del núcleo solo leen y escriben las cadenas (conservando las demás llaves); `cli/` las interpreta con `resolve_appearance`: un id o fondo desconocido usa el valor por defecto con un aviso en la línea de estado.
- **Key de búsqueda web (Tavily):** `CHAT_SEARCH_API_KEY` (entorno) tiene precedencia; si no, `credentials.json` con la llave `"search:tavily"` (`kSearchCredentialsKey`), con el mismo archivo, permisos, escritura atómica y validación de forma (`plausible_key`) que la del modelo. **Nunca** en `config.json`. `load_search_api_key(env, credentials_path) -> Result<std::string>`: vacía si no hay archivo o llave; permisos abiertos o JSON inválido → error `Config`. Sin key, `/buscar` no se envía: aviso "Configura la key de búsqueda en Configuración (F2) → Búsqueda web" y el texto se queda en la caja. La pantalla de configuración la edita en la categoría "Búsqueda web" (enmascarada con `mask_key`, bloqueada por `CHAT_SEARCH_API_KEY`).
- **Instrucciones de sistema:** `"system_prompt"` de `config.json`, una cadena y global (todas las conversaciones, también las que se cargan; no se guarda por conversación). Llave ausente: `kDefaultSystemPrompt`; `""`: sin mensaje de sistema; otro tipo: el predeterminado con un aviso en la línea de estado. `load_system_prompt_value`/`save_system_prompt` del núcleo solo leen y escriben la cadena (otro tipo → error `Config`; `nullopt` borra la llave, conservando las demás); `cli/` la interpreta y la valida al guardar (`system_prompt.h`: máximo 8000 bytes, UTF-8 válido, sin controles salvo `\n` y `\t`; aviso si mide `history_limit` o más). Al guardar se aplica a la conversación abierta con `Conversation::set_system_prompt` (desde el siguiente mensaje). Sin variable de entorno.
- **Conversaciones guardadas:** un archivo `<id>.json` por conversación (esquema versión 1; `id` = `AAAAMMDD-HHMMSS-xxxxxx`), con la carpeta en 0700 y los archivos en 0600. No se guarda el mensaje de sistema (al cargar se anteponen las instrucciones de sistema vigentes, salvo que estén vacías); cada respuesta guarda su `model` y su `finish_reason`. `messages` son pares `user` → `assistant` alternados: empieza en `user`, termina en `assistant` y tiene al menos un par; si no (dos `user` o dos `assistant` seguidos, empezar en `assistant`, terminar en `user`, lista vacía o un rol que no sea `user` ni `assistant`, incluido `system`), el archivo es ilegible: `load` da el error en español ("hay dos mensajes del usuario seguidos", "el último mensaje del usuario no tiene respuesta"...) y `list` lo marca. Así un archivo editado a mano nunca manda dos `User` seguidos ni un segundo `System`. Un mensaje de usuario hecho con `/buscar` guarda el texto escrito y la llave opcional `"search": {"query", "date": "AAAA-MM-DD", "results": [{"title", "url", "content", "published_date"}]}` con el content ya recortado; al cargar se reconstruye el bloque con `format_search_context` (fecha de `date`, o de `created_at` si falta; nonce nuevo) y las fuentes. El lector ignora llaves desconocidas, así que sigue siendo la versión 1; una `"search"` mal formada hace ilegible el archivo, pero un resultado cuya `url` no pasa `is_web_url` (por ejemplo `javascript:` o `file:`) solo se descarta, igual que en `TavilySearch` (si no queda ninguno, no hay entrada "Fuentes"). Se guarda solo tras cada par usuario/asistente terminado, con escritura atómica (`write_file_atomic`: `.tmp` + `fsync` + `rename` + `fsync` del directorio; en Windows, `MoveFileExW`); la carpeta se crea en 0700 si falta, y si ya existía (`CHAT_DATA_DIR`) no se tocan sus permisos. Un archivo ilegible o de una versión desconocida se lista pero nunca se sobrescribe ni se borra.
- `CHAT_DEBUG_SSE=/ruta/archivo` (`Config::debug_sse_path`): `ChatClient` agrega al archivo, por cada intento de `complete` y de `complete_stream`, fecha y hora, modelo, número de intento, estado HTTP y el cuerpo crudo de la respuesta, más una línea separadora. Nunca escribe cabeceras, la key ni el cuerpo de la petición. Si no se puede abrir o escribir, se ignora en silencio. El archivo contiene la conversación: solo para diagnosticar.

## 8. HTTP y API

- Cuerpo de la petición: `model`, `messages` (cada uno con `role` y `content`), `stream: false` en el hito 1.
- Respuesta: leer `choices[0].message.content`. Validar que cada campo exista; si falta algo, devolver `BadResponse`, nunca tronar ni acceder a un índice inexistente.
- **Mapeo de estados HTTP:**
  - 2xx: éxito.
  - 401 y 403: `Auth`.
  - 404 y 410: `ModelNotFound`.
  - 400, 413 y 422: `InvalidRequest` (por ejemplo, "maximum context length exceeded").
  - 429: `RateLimited`.
  - 5xx: `Server`.
  - Otros estados no exitosos: `BadResponse`.
  - `Retry-After` (segundos enteros, con tope de 60 s) se respeta en todos los errores HTTP reintentables: 429 y 5xx.
  - **Errores dentro del flujo** (evento SSE `{"error": {...}}` con estado 200, como manda NIM la sobrecarga): si `error.code` es un número o una cadena numérica, se clasifica con el mismo mapeo de estados HTTP; si no, por `error.type` sin distinguir mayúsculas (`overload`, `unavailable` o `server` → `Server`; `rate` o `exhausted` → `RateLimited`); si no, por `error.message` sin distinguir mayúsculas (`overload` o `temporarily unavailable` → `Server`; `rate limit` o `too many requests` → `RateLimited`); si nada aplica, `BadResponse`. El mensaje es `error.message`, truncado. `complete_stream` los reintenta con la regla de siempre: solo si son reintentables y no se ha entregado ningún delta.
  - Fallo de curl: `Network` o `Timeout` según el código.

### Tavily (búsqueda web)

- `POST https://api.tavily.com/search`, `Authorization: Bearer <key>` (por `HttpRequest::api_key` y `CurlTransport`), timeout de 30 s. Cuerpo fijo: `query`, `search_depth: "basic"` (1 crédito), `max_results: 5`, `topic: "general"`, `safe_search: true` (no configurable: los usuarios son menores de edad), `country: "mexico"`, `include_published_date: true` (sin él, con `topic: "general"` no llega la fecha). Sin `include_answer`, `include_raw_content` ni imágenes.
- Respuesta: `results` debe ser arreglo (si no, `BadResponse`); se descarta un resultado sin `url` cadena que pase `is_web_url` o sin `content` cadena; `title` y `published_date` son opcionales; lo demás se ignora. `published_date` llega como RFC 1123 (`"Tue, 11 Mar 2025 17:00:00 GMT"`) o `null`; `normalize_published_date` (`core/src/published_date.h`, pura) la deja como `AAAA-MM-DD` si es ISO 8601 o RFC 1123 con fecha válida, y si no, tal cual. "Fuentes" la muestra: `[n] título — AAAA-MM-DD (url)`.
- Errores (el cuerpo es `{"detail": {"error": "..."}}`): 400 y 422 → `InvalidRequest`; 401 → `Auth` ("La key de búsqueda no es válida"); 429 → `RateLimited`; 432 y 433 → `RateLimited` ("Se agotaron las búsquedas del plan de Tavily"); 5xx → `Server`; otros → `BadResponse`. Sin respuesta HTTP (estado 0): `cancelled` o token cancelado → `Cancelled`; `timed_out` → `Timeout`; si no, `Network`, con un mensaje propio si el transporte no dio uno (nunca "Error HTTP 0"). Sin reintentos ni volcado de depuración.
  - Cuerpo que no se puede interpretar: `BadResponse`.
- En errores, intentar extraer el mensaje del cuerpo (`error.message` o `detail`); si no hay, usar el texto del estado HTTP. Truncar cuerpos largos.
- `base_url` se valida interpretando la URL: `https://` siempre; `http://` **solo** si el host es exactamente `localhost`, `127.0.0.1` o `[::1]` (cualquier puerto), para servidores locales como Ollama o llama.cpp; nunca con `usuario@` en la URL. Con host local la key es opcional y, si está vacía, `CurlTransport` no manda `Authorization`.
- Verificación TLS **siempre activa**. Timeout de conexión 10 s.
- `curl_global_init` se llama una sola vez (envuélvelo en RAII).

## 9. Seguridad y secretos

- La key nunca se escribe en archivos (salvo `credentials.json`, con 0600; en Windows, protegido por la ACL del perfil en `%APPDATA%`, sin cifrar hasta W3 con DPAPI), logs, volcados, mensajes de error, pruebas ni en el repositorio, ni se dibuja en claro.
- TLS en Windows: `CURLSSLOPT_NATIVE_CA` usa el almacén de certificados de Windows (sin repartir `cacert.pem`); la verificación sigue siempre activa.
- Si se registra una cabecera `Authorization`, se redacta.
- El texto de las páginas que trae la búsqueda web es **datos, no instrucciones**: va dentro del bloque con nonce aleatorio por búsqueda, limpio de controles, con la cadena de cierre neutralizada y con la instrucción de ignorar órdenes que aparezcan dentro. Nunca va en un mensaje `System`. A Tavily solo se envía la consulta, nunca la conversación. Las URL de "Fuentes" salen de la búsqueda (no del texto del modelo), pasan por `is_web_url` al recibirlas y al cargarlas (y otra vez al dibujarlas: si no pasa, sin enlace) y por `md::sanitize` y `hyperlink_target`.
- `.gitignore` debe cubrir directorios de build y cualquier archivo local de configuración o claves.
- **Las pruebas nunca llaman a la API real.** `tools/smoke.cpp` es la única excepción y solo corre a mano.

## 10. Estilo de código

- RAII para todo recurso de curl (`CURL*`, `curl_slist`). Sin `new`/`delete` manuales.
- Sin `using namespace` en headers. Sin variables globales mutables.
- `const` donde se pueda; `[[nodiscard]]` en funciones que devuelven `Result`.
- Identificadores en inglés (`snake_case` para funciones y variables, `PascalCase` para tipos). Comentarios y documentación en español. Mensajes de error para el usuario en español.
- Headers públicos mínimos: lo que no necesite verse desde fuera va en `src/`.

## 11. Compilación y verificación

Dependencias en Fedora:

```bash
sudo dnf install gcc-c++ cmake ninja-build libcurl-devel json-devel libasan libubsan libtsan
```

Flags de warnings en todos los presets: `-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror`.

Presets (con su preset de build y de test del mismo nombre):

- `dev`: Debug con warnings.
- `asan`: Debug con AddressSanitizer + UndefinedBehaviorSanitizer.
- `tsan`: Debug con ThreadSanitizer (desde el hito 3: la interfaz usa un hilo de trabajo).
- `release`: Release (`-O3 -DNDEBUG`, el valor de CMake) con los mismos warnings y `-Werror`. Es la build para usar el chatbot a diario (`./build/release/cli/chatbot`); sus pruebas también deben pasar. Con `NDEBUG` no hay `assert`: ninguna prueba debe depender de ellos.

`asan` y `tsan` ponen los sanitizadores también en `CMAKE_C_FLAGS`, para que md4c (C) quede instrumentado. Los flags de warnings solo se aplican a nuestros targets (`chatbot_set_warnings`), no a md4c ni a FTXUI.

Comandos de verificación:

```bash
cmake --preset dev  && cmake --build --preset dev  && ctest --preset dev  --output-on-failure
cmake --preset asan && cmake --build --preset asan && ctest --preset asan --output-on-failure
cmake --preset tsan && cmake --build --preset tsan && ctest --preset tsan --output-on-failure
cmake --preset release && cmake --build --preset release && ctest --preset release --output-on-failure
```

Las pruebas de `RequestRunner` y de `CancelToken` crean hilos (deben pasar con `tsan`). Aun así, corre también `./build/tsan/cli/chatbot` con una conversación real, cancelando un par de veces, y revisa que no aparezca `WARNING: ThreadSanitizer`: el loop de FTXUI no está en las pruebas.

La CI (`.github/workflows/ci.yml`, en cada PR y en cada push a `main`) corre estos mismos comandos con GCC en `dev`, `asan`, `tsan` y `release`, y con Clang en `dev` y `release`. Además, comprueba que el ejecutable sin `CHAT_API_KEY` salga con código 1. **La CI nunca usa API keys ni llama a una API real.** En el trabajo `tsan` pone `vm.mmap_rnd_bits=28`, porque con 32 TSan falla al arrancar.

**Windows** (MSYS2 UCRT64, en la terminal "MSYS2 UCRT64"):

```bash
pacman -S --needed git mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,curl,nlohmann-json,catch}
cmake --preset dev     && cmake --build --preset dev     && ctest --preset dev     --output-on-failure
cmake --preset release && cmake --build --preset release && ctest --preset release --output-on-failure
```

El trabajo `windows` de la CI (`windows-2025`, `msys2/setup-msys2` con `UCRT64`) corre esos dos presets, sin `CHATBOT_STRICT_PERF` (los runners de Windows varían; la latencia de cancelación < 2 s se exige igual), no toca `core.autocrlf` (prueba lo que obtiene quien clona con Git for Windows; los saltos de línea los fija `.gitattributes`: LF en todo, `.ans` sin conversión porque llevan secuencias de escape, `.cmd`/`.bat` en CRLF para CMD), imprime `curl -V` (versión y backend TLS: el curl de MSYS2 UCRT64 usa OpenSSL), revisa el arranque sin key con stdin en `NUL` y `timeout`, y en release sube el artefacto `chatbot-windows-x64` (`chatbot.exe` con las DLL de `/ucrt64/bin` que reporta `ldd`) para probar en una PC sin MSYS2. Las pruebas de permisos POSIX (0600, 0700, umask, "chmod 600") y de enlaces simbólicos no se compilan en Windows; ahí corren las de Windows (credenciales sin `chmod`, rutas con `\`).

`ctest` corre dos ejecutables: `chatbot_tests` (núcleo y `chatbot_cli_lib`, sin FTXUI) y `chatbot_ui_tests` (vista de markdown e historial y temas, dibujando con `ftxui::Screen`, sin terminal). `tests/data/markdown_muestra_terminal_w40.ans` y `_w80.ans` son `Screen::ToString()` del render con el tema "De la terminal" (24 bits); si cambia el render a propósito, se regeneran con la salida de `md_preview --theme terminal` en una terminal de 24 bits, quitando el `\r` extra que agrega la pty y el salto de línea final. Todas las pruebas de tiempo miden y reportan siempre con `WARN`. Las de latencia de cancelación (`CancelToken`, `RealSleeper` y `RequestRunner`, incluido su destructor; `report_latency` en `tests/performance.hpp`) exigen **< 2 s en todos los presets**, sin depender de `CHATBOT_STRICT_PERF`: miden la respuesta a la cancelación, no la velocidad del equipo. Los límites de las demás (parseo, reparto de tablas, historial) solo fallan con `CHATBOT_STRICT_PERF=1`, que la CI activa únicamente en release (GCC y Clang). Las esperas máximas entre hilos y la comprobación de no terminar una espera antes de tiempo son verificaciones funcionales y siguen siendo obligatorias. Para revisar el render a ojo: `./build/dev/tools/md_preview --width 40 tests/data/markdown_muestra.md`.

## 12. Pruebas (Catch2)

Para el hito 1, sin red, usando `FakeTransport`:

- **Configuración:** precedencia entorno sobre archivo, falta de key, falta de modelo, archivo inexistente, archivo con JSON inválido, `XDG_CONFIG_HOME` presente y ausente.
- **Mapeo de errores:** una prueba por cada fila de la tabla de la sección 8.
- **Respuesta:** válida, sin `choices`, `choices` vacío, sin `content`, JSON malformado, cuerpo de error con y sin `error.message`.
- **Reintentos:** cuenta de intentos, respeto de `Retry-After`, espera exponencial, y que `Auth`/`ModelNotFound` no reintenten. Con `Sleeper` falso.
- **Petición:** el cuerpo JSON generado tiene el modelo y los mensajes en el orden y con los roles correctos.

## 13. Definición de "hecho" (hito 1)

- [ ] Compila sin warnings con GCC (y con Clang si está instalado).
- [ ] `ctest` pasa en los presets `dev` y `asan`, sin reportes de sanitizadores.
- [ ] `tools/smoke.cpp` manda un mensaje real con la key del usuario y muestra la respuesta o el tipo de error (se ejecuta a mano, con permiso del usuario).
- [ ] Ningún secreto en el repositorio ni en la salida.
- [ ] No hay código de hitos posteriores.
- [ ] Informe final con la salida real de los comandos de la sección 11 y la lista de dudas pendientes.
