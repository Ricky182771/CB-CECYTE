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

**Nota para el hito 4: prueba con hilos** (sin loop de FTXUI). Implementada con `RequestRunner` (`cli/src/request_runner.*`) y `tests/test_request_runner.cpp`; el transporte que se bloquea quedó aparte, en `tests/blocking_transport.hpp`:

- Extraer el hilo de trabajo de `cli/src/main.cpp` a una clase sin FTXUI que reciba una función para pasar tareas al hilo de la interfaz. En producción esa función hace `screen.Post` + `PostEvent(Event::Custom)`; en las pruebas, mete las tareas en una cola que la prueba vacía.
- Agregar a `FakeTransport` un bloqueo controlable para detener el flujo a la mitad.
- Correr la prueba con el preset `tsan`.

Todo lo de hitos posteriores está **fuera de alcance** del hito actual. No lo adelantes.

## 4. Stack

- **Lenguaje:** C++20. No usar funciones de C++23 (por ejemplo, `std::expected`).
- **Build:** CMake 3.21 o superior, con presets. Generador Ninja.
- **Compiladores:** GCC y Clang deben compilar sin warnings.
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
├── .github/workflows/ci.yml  # CI: GCC (dev, asan, tsan, release) y Clang (dev, release)
├── core/
│   ├── CMakeLists.txt
│   ├── include/chatbot/     # headers públicos
│   └── src/
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
│       ├── markdown.h/.cpp            # markdown → árbol propio con md4c, y filtrado del texto (sin FTXUI)
│       ├── markdown_view.h/.cpp       # árbol de markdown → ftxui::Element (chatbot_cli_ui)
│       ├── history_view.h/.cpp        # entradas de la conversación, con caché (chatbot_cli_ui)
│       ├── input_style.h/.cpp         # estilo de la caja de entrada, sin invertido (chatbot_cli_ui)
│       ├── sidebar_view.h/.cpp        # dibujo de la barra de conversaciones (chatbot_cli_ui)
│       └── main.cpp             # interfaz FTXUI (ejecutable chatbot)
└── tests/                   # chatbot_tests (sin FTXUI) y chatbot_ui_tests (vista)
    └── data/                # markdown_muestra.md: muestra con todos los elementos
```

- Biblioteca estática `chatbot_core`, namespace `chatbot`.
- `core/` no puede depender de FTXUI ni leer/escribir en la terminal (salvo `tools/`).
- `chatbot_cli_lib` (biblioteca estática de `cli/`) solo depende de `chatbot::core` (y, en privado, de nlohmann/json y md4c), sin FTXUI, para poder probarla.
- `chatbot_cli_ui` (biblioteca estática de `cli/`) dibuja sin terminal: enlaza `chatbot_cli_lib`, `ftxui::dom` y `ftxui::screen`. La usan el ejecutable `chatbot`, `tools/md_preview` y `chatbot_ui_tests`. Solo el ejecutable `chatbot` enlaza `ftxui::component`.
- Conversaciones guardadas: una barra lateral a la izquierda (lógica en `Sidebar`, que reusa `ConversationList`; dibujo en `sidebar_view`). Ctrl+B la muestra u oculta, Ctrl+O le da el foco y Ctrl+N empieza una conversación nueva. Se muestra al arrancar con 100 columnas o más y su borde derecho es el divisor de `ResizableSplit`. Reemplaza a la lista de pantalla completa.
- Todo texto que viene del modelo, del usuario o de un archivo pasa por `md::sanitize` antes de dibujarse (controles C0 salvo `\n`, ESC, DEL y C1 → U+FFFD; tab → 4 espacios). FTXUI descarta esos caracteres en `text()`, pero sin dejar rastro, y escribe sin filtrar la URL de `hyperlink`: por eso las URL además se codifican con `hyperlink_target`.

## 6. Diseño del núcleo

- **Tipos básicos:** `Role` (`System`, `User`, `Assistant`), `Message { Role role; std::string content; }`, `Config`.
- **Errores:** `ChatError { ErrorKind kind; int http_status; std::string message; std::optional<std::chrono::seconds> retry_after; }`.
  `ErrorKind`: `Config`, `Auth`, `ModelNotFound`, `RateLimited`, `Server`, `Network`, `Timeout`, `BadResponse`, `InvalidRequest`, `Cancelled`.
- **Resultado:** `Result<T>` mínimo sobre `std::variant<T, ChatError>`. Las excepciones pueden usarse internamente, **pero nunca cruzan la API pública**.
- **Transporte:** interfaz `Transport` que hace el POST y devuelve estado HTTP, cabeceras relevantes (`Retry-After`) y cuerpo, o un error de red. `CurlTransport` es la implementación real; las pruebas usan un `FakeTransport`. Diséñala para poder agregar streaming en el hito 2 sin romper `complete`; si hace falta cambiar algo, pregunta.
- **Cliente:** `ChatClient(Config, std::unique_ptr<Transport>)` con `complete(const std::vector<Message>&, const CancelToken* = nullptr) -> Result<std::string>` y `complete_stream(messages, on_delta, const CancelToken* = nullptr) -> Result<CompletionInfo>`. `CompletionInfo { std::string finish_reason; }` (`chatbot/completion_info.h`): el último `choices[0].finish_reason` no nulo que llegó, o vacío si nunca llegó.
- **Cancelación:** `CancelToken` (`cancel()`, `is_cancelled()`, `wait_for(ms)`; seguro entre hilos, no copiable ni movible; quien lo crea lo mantiene vivo durante la petición). `ChatClient` lo revisa antes y después de cada intento (una cancelación durante un intento gana aunque haya terminado bien), lo pasa al transporte (`HttpRequest::cancel`) y a la espera de reintento; si se cancela, el resultado es `Cancelled` y no se reintenta. En la interfaz, `RequestRunner::cancel()` gana mientras `busy()` sea `true`, aunque la respuesta ya haya terminado en el hilo de trabajo. `CurlTransport` lo revisa desde `CURLOPT_XFERINFOFUNCTION`, que libcurl llama alrededor de una vez por segundo aunque no lleguen datos: la latencia de cancelación es de ~1 s.
- **Reintentos** (dentro de `ChatClient`):
  - Reintentar solo `RateLimited`, `Server`, `Network` y `Timeout`.
  - Máximo 3 reintentos, con espera exponencial desde 1 s. Si hay `Retry-After`, respetarlo.
  - Nunca reintentar `Auth`, `ModelNotFound`, `BadResponse`, `InvalidRequest`, `Config` ni `Cancelled`.
  - La espera se inyecta (interfaz `Sleeper`: `bool sleep_for(std::chrono::milliseconds, const CancelToken*)`, que devuelve `false` si la cancelación interrumpió la espera) para que las pruebas no esperen de verdad.
- El historial lo administra quien llama; el núcleo no lo guarda.
- **Recorte del historial:** `trim_history(messages, limit_bytes) -> TrimResult{messages, dropped}` (función pura, `chatbot/history.h`). Nunca quita los mensajes `System` iniciales ni el último; quita los más viejos en pares usuario+asistente hasta caber en el límite (bytes UTF-8 de los `content`). `0` = sin límite.

## 7. Configuración

Precedencia: **variables de entorno > archivo de configuración > valores por defecto.**

| Dato | Variable | Archivo | Por defecto |
|------|----------|---------|-------------|
| API key | `CHAT_API_KEY` | **nunca** | ninguno (error `Config` si falta) |
| URL base | `CHAT_BASE_URL` | `base_url` | `https://integrate.api.nvidia.com/v1` |
| Modelo | `CHAT_MODEL` | `model` | **ninguno** (error `Config` con mensaje claro si falta) |
| Timeout (s): total en `complete`, por inactividad en `complete_stream` | `CHAT_TIMEOUT` | `timeout_seconds` | 120 |
| Límite del historial que se envía, en bytes UTF-8 de los `content` (aproximadamente caracteres); `0` = sin límite | `CHAT_HISTORY_LIMIT` | `history_limit` | 32000 |
| Archivo de volcado de depuración | `CHAT_DEBUG_SSE` | **nunca** | ninguno (sin volcado) |
| Carpeta de conversaciones guardadas (la lee `cli/`, no `Config`) | `CHAT_DATA_DIR` | **nunca** | `$XDG_DATA_HOME/chatbot/conversations` si `XDG_DATA_HOME` es ruta absoluta; si no, `~/.local/share/chatbot/conversations` |

- Archivo: `$XDG_CONFIG_HOME/chatbot/config.json`, o `~/.config/chatbot/config.json` si no existe esa variable. Que el archivo no exista no es un error.
- **Sin modelo por defecto a propósito:** los modelos de NIM se retiran con el tiempo y un nombre fijo en el código acabaría roto.
- La key **solo** viene del entorno.
- **Conversaciones guardadas:** un archivo `<id>.json` por conversación (esquema versión 1; `id` = `AAAAMMDD-HHMMSS-xxxxxx`), con la carpeta en 0700 y los archivos en 0600. No se guarda el mensaje de sistema (al cargar se antepone el `kSystemPrompt` actual); cada respuesta guarda su `model` y su `finish_reason`. Se guarda solo tras cada par usuario/asistente terminado, con escritura atómica (`.tmp` + `fsync` + `rename` + `fsync` del directorio). Un archivo ilegible o de una versión desconocida se lista pero nunca se sobrescribe ni se borra.
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
  - Cuerpo que no se puede interpretar: `BadResponse`.
- En errores, intentar extraer el mensaje del cuerpo (`error.message` o `detail`); si no hay, usar el texto del estado HTTP. Truncar cuerpos largos.
- Verificación TLS **siempre activa**. Timeout de conexión 10 s.
- `curl_global_init` se llama una sola vez (envuélvelo en RAII).

## 9. Seguridad y secretos

- La key nunca se escribe en archivos, logs, mensajes de error, pruebas ni en el repositorio.
- Si se registra una cabecera `Authorization`, se redacta.
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

`ctest` corre dos ejecutables: `chatbot_tests` (núcleo y `chatbot_cli_lib`, sin FTXUI) y `chatbot_ui_tests` (vista de markdown e historial, dibujando con `ftxui::Screen`, sin terminal). Las pruebas de desempeño solo exigen tiempos en la build sin sanitizadores; con sanitizadores solo los muestran (`WARN`). Para revisar el render a ojo: `./build/dev/tools/md_preview --width 40 tests/data/markdown_muestra.md`.

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
