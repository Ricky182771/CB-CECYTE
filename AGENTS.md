# AGENTS.md — Reglas del proyecto Chatbot (terminal, núcleo en C++)

> Lee este archivo completo antes de escribir cualquier código. Si una instrucción del usuario contradice algo de aquí, gana el usuario, pero avísale de la contradicción antes de continuar.

## 1. Qué es el proyecto

Un chatbot tipo asistente general que corre en la terminal (pantalla completa, estilo OpenCode). Proyecto escolar en equipo, de alcance chico.

- **Núcleo (`core/`)**: biblioteca C++ con cliente HTTP, historial, configuración y manejo de errores. No sabe nada de terminales ni de interfaces.
- **Markdown (`markdown/`)**: biblioteca `chatbot::markdown` (parseo con md4c, filtrado del texto, bloques de primer nivel con su rango en bytes). Sin FTXUI: la usan la interfaz y los addons.
- **Interfaz (`cli/`)**: ejecutable con FTXUI. **No se toca hasta el hito 3.**
- **RPC (`rpc/`)**: ejecutable `chatbot-rpc`, la app sin pantalla por JSON-RPC 2.0 sobre stdin/stdout, para la interfaz gráfica (Flutter) y otras interfaces. Usa la misma lógica que la TUI (`Session`), sin FTXUI ni addons. Protocolo en `rpc/PROTOCOL.md`.
- **Addons (`addons/`)**: bots que usan el mismo núcleo (hoy, Telegram), compilados por defecto solo en Linux; la app los arranca (`chatbot telegram`) y los configura en F2 → Addons (sección 14). `core` no sabe que existen; de `addons/`, `cli/` solo usa `chatbot::addon_settings`.
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

### Antes de tocar un módulo

Lee la guía correspondiente antes de hacer cambios:

| Antes de tocar | Lee |
|---|---|
| Hitos y alcance | `guias/HISTORIAL.md` |
| Dependencias y plataforma de compilación | `guias/04-stack.md` |
| `cli/` (TUI), `Session`, plataforma, pegado, ajuste de línea o símbolos | `guias/05-estructura.md` |
| `markdown/` y render | `guias/05-estructura.md` |
| `core/` | `guias/06-nucleo.md` y `guias/08-http.md` |
| Configuración, credenciales o persistencia | `guias/07-configuracion.md` |
| Addons | `guias/14-addons.md` |
| `rpc/` | `rpc/PROTOCOL.md` y `guias/05-estructura.md` |
| Presets, compilación, pruebas o CI | `guias/11-compilacion.md` |

## 3. Alcance por hitos

El alcance de cada PR lo fija su prompt. Todo lo de hitos posteriores está fuera de alcance del hito actual. No lo adelantes. La tabla completa de hitos está en `guias/HISTORIAL.md`.

Detalle: guias/HISTORIAL.md

## 4. Stack

- C++20, sin C++23. CMake ≥ 3.21, Ninja y presets; GCC y Clang sin warnings.
- Windows: MSYS2 UCRT64 con GCC y `-Werror`, sin MSVC; Windows 10 IoT LTSC 21H2 (19044)/conhost y Windows 11/Windows Terminal. Definiciones Win32 solo en nuestros targets y con `WIN32`; sanitizadores solo Linux.
- Permitidas: libcurl; nlohmann/json ≥ 3.9 (3.x); Catch2 v3 solo pruebas; FTXUI v7.0.3 solo interfaz; md4c v0.6.0 solo markdown, estática, headers `SYSTEM`, siempre FetchContent con `GIT_TAG v0.6.0`. Bibliotecas del sistema Windows no cuentan.
- `find_package` y, si falta, FetchContent con versión fija, nunca `master`/`main`.
- **Excepción FTXUI:** siempre FetchContent al commit `f921fad208912747c17d129a8ef75ec7624b6eec` (v7.0.3), sin `find_package` ni `GIT_SHALLOW`.
  El parche `cmake/patches/ftxui-7.0.3-drain-input.patch` (#1351) evita retrasos de entrada POSIX; ninguna versión publicada lo trae.
  Aplicación y retirada del parche: `guias/04-stack.md`.
- Zonas C++20 con tzdata del sistema: `CHATBOT_TZDB=AUTO/ON/OFF`, detección compilando/enlazando. Sin base (siempre Windows), solo desfase fijo, por defecto `kDefaultUtcOffset`; CMake elige implementación.
- **Ninguna otra dependencia sin preguntar.**

Detalle: guias/04-stack.md

## 5. Estructura de directorios

```text
chatbot/
├── .github/
│   └── workflows/
├── cmake/
│   ├── windows/
│   └── patches/
├── core/
│   ├── include/
│   └── src/
├── markdown/
│   ├── include/
│   └── src/
├── cli/
│   └── src/
├── rpc/
│   ├── src/
│   └── tests/
├── addons/
│   ├── settings/
│   ├── common/
│   └── telegram/
├── tools/
│   └── windows-dist/
├── tests/
│   └── data/
└── guias/
```

- `chatbot_core` (namespace `chatbot`) no depende de FTXUI ni addons, ni usa la terminal. `chatbot_markdown` (`chatbot::markdown`, namespace `chatbot::md`) no depende de core ni FTXUI; md4c en privado. El dibujo de markdown también usa `chatbot::md`.
- `chatbot_cli_lib` solo depende de `chatbot::core`, `chatbot::markdown`, `chatbot::addon_settings` y nlohmann/json privado; sin FTXUI ni includes de `core/src`.
- RPC solo depende de `chatbot_cli_lib` y nlohmann/json; nunca FTXUI, `chatbot_cli_ui` ni addons. Siempre se compila, también en Windows y sin addons. **Todo cambio de protocolo se documenta en `rpc/PROTOCOL.md` en el mismo PR**, incluida su tabla de estado.
- `chatbot_cli_ui`: `chatbot_cli_lib`, `ftxui::dom` y `ftxui::screen`; dibuja sin terminal. Solo `chatbot` y `chatbot_ui_tests` enlazan `ftxui::component`.
- **Lo nuevo que no dependa de FTXUI va en `Session`, no en `main.cpp`.** Solo desde el hilo de interfaz, con entorno inyectado. TUI y RPC comparten arranque. `Session` se destruye antes de `screen`.
- Plataforma elegida por CMake, sin `#ifdef` en lógica; `<windows.h>` solo en `.cpp` de Windows. Strings UTF-8, llamadas Win32 W; escritura atómica común.
- Colores solo en `Theme`/`Palette`, símbolos por su tabla; nunca sustituir símbolos del texto externo. La caché incluye `Palette::key()`. Texto externo por `md::sanitize`, URL además por `hyperlink_target`.
- Preguntas s/n: `yes_no_answer`, s/S sí, n/N no, Esc cancela con aviso; lo demás se ignora y ningún atajo las salta. `kCommands` es el registro único. `hit_test` usa el cuadro anterior, sin `render` intermedio.
- Descargas sin sobrescribir ni hacer ejecutables, nombres válidos también en Windows; procesos sin shell. Exportar solo pares guardados, sin sistema, resultados, avisos ni errores.

Detalle: guias/05-estructura.md

## 6. Diseño del núcleo

- `Result<T>` sobre `std::variant<T, ChatError>`; **las excepciones nunca cruzan la API pública**. Errores tipados por `ErrorKind`, nunca por su texto.
- `Transport` separa HTTP de `ChatClient`; producción con curl, pruebas con `FakeTransport`. El historial lo administra quien llama.
- `CancelToken` seguro entre hilos, no copiable ni movible, vivo durante la petición. Cancelación antes/después del intento, transporte y espera; gana aunque el intento termine bien y, en `RequestRunner`, mientras `busy()` sea true.
- Solo reintentar `RateLimited`, `Server`, `Network`, `Timeout`: máximo 3, espera exponencial desde 1 s, `Retry-After` e inyección de espera. Nunca errores permanentes, cancelación ni `NoSearchResults`.
- Recorte puro por bytes UTF-8: conserva `System` iniciales y último mensaje, quita pares viejos; 0 sin límite. `complete_trimmed` comparte el recorte y un reintento por contexto, solo con `InvalidRequest`, sin deltas ni cancelación y si quita más mensajes.
- Búsqueda fallida, cancelada o vacía no llama al modelo. `NoSearchResults` es aviso. Bloque limpio y recortado con nonce reemplaza el último `User`, nunca otro `User` ni `System`; usa la fecha guardada.

Detalle: guias/06-nucleo.md

## 7. Configuración

- **Entorno > archivo > valores por defecto.** Sin modelo por defecto; archivo ausente no es error. Keys solo en entorno o `credentials.json` por proveedor, **nunca `config.json`**; Tavily usa `search:tavily`.
- POSIX: credenciales 0600, carpeta 0700; permisos abiertos son error `Config`. Windows usa ACL del perfil, sin comprobaciones POSIX. Guardado atómico, conservando claves ajenas y orden.
- Rutas normales XDG o las del usuario; Windows usa AppData e ignora `HOME`/`XDG_*`. `CHAT_DATA_DIR` y `CHAT_DOWNLOAD_DIR` prevalecen.
- Portable: `chatbot.portable` junto al ejecutable lleva los datos a `datos/`, gana sobre rutas del usuario, no sobre `CHAT_*`; descargas iguales. Probar escritura al arrancar: si falla, la TUI sale con el error `Config` y código 1, y `chatbot-rpc` responde `StartupFailed` (-32011) en `initialize`, sin salir; nunca volver en silencio a la carpeta del usuario.
- `system_prompt` global, sin entorno ni copia por conversación: ausente usa el predeterminado, vacío omite `System`. UTF-8, máximo 8000 bytes, sin controles salvo salto/tabulador; guardar aplica desde el siguiente mensaje.
- Apariencia sin entorno; valores desconocidos usan predeterminados con aviso. Símbolos siguen configurables con `NO_COLOR`.
- Conversaciones versión 1: al menos un par user/assistant alternado completo, nunca `system` persistido. Guardar tras cada par, atómicamente, carpeta nueva 0700 y archivo 0600. Archivo ilegible o de versión desconocida se lista, nunca se sobrescribe ni borra. Cargar usa sistema vigente, búsqueda con nonce nuevo y fecha guardada.
- Sin key de búsqueda, no enviar `/buscar` ni vaciar la caja. `CHAT_SEARCH_API_KEY` gana sobre credenciales, con la misma privacidad y validación.
- Solo la app escribe `addons.<addon>` en `config.json`; Telegram ausente queda desactivado. Versión mayor: F2 solo lectura; descartar llaves requiere confirmación; bot rechaza sección inválida. Secretos: §14.
- Diagnósticos solo por entorno: `CHAT_DEBUG_SSE` nunca vuelca cabeceras, key ni petición; `CHAT_DEBUG_KEYS` nunca el texto de caracteres.

Detalle: guias/07-configuracion.md

## 8. HTTP y API

- Validar campos y JSON: ausentes o inválidos dan `BadResponse`, sin acceder a índices inexistentes.
- HTTP: 2xx éxito; 401/403 `Auth`; 404/410 `ModelNotFound`; 400/413/422 `InvalidRequest`; 429 `RateLimited`; 5xx `Server`; demás no exitosos `BadResponse`. Curl: `Network` o `Timeout`. Mensajes truncados, sin secretos.
- **Nunca seguir redirecciones**: 3xx sin reintento; destino/base sugerida solo saneados, sin key. `Retry-After` entero, tope 60 s en 429/5xx. Errores SSE con HTTP 200 también se clasifican; reintentar streaming solo antes de entregar deltas.
- URL sin usuario; HTTPS, o HTTP solo con host exacto `localhost`, `127.0.0.1`, `[::1]`. Solo ahí key opcional; vacía no manda `Authorization`. TLS siempre verificado, conexión 10 s, `curl_global_init` una vez con RAII.
- Tavily: consulta sola, timeout 30 s, basic, máximo 5, general, **safe_search true no configurable**, país México, fecha; sin respuesta generada, contenido crudo ni imágenes. Sin reintentos ni volcado. Solo resultados con URL web/content válidos; 432/433: cuota agotada.

Detalle: guias/08-http.md

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

Flags en todos los presets: `-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror`, solo en nuestros targets. `asan` y `tsan` instrumentan también md4c mediante `CMAKE_C_FLAGS`.

Presets de configure, build y test con el mismo nombre:

- `dev`: Debug con warnings.
- `asan`: Debug con AddressSanitizer y UndefinedBehaviorSanitizer.
- `tsan`: Debug con ThreadSanitizer.
- `release`: Release (`-O3 -DNDEBUG`), mismos warnings y pruebas; ninguna prueba depende de `assert`.
- En Linux todos incluyen Telegram por defecto. `dev-telegram` y `release-telegram` ya no existen; una caché con addons apagados necesita `-DCHATBOT_BUILD_TELEGRAM=ON` para encenderlos.
- **Poca memoria (6 GB):** compilar con `cmake --build --preset asan -j2` y probar con `ctest --preset asan -j1`.

Comandos de verificación:

```bash
cmake --preset dev  && cmake --build --preset dev  && ctest --preset dev  --output-on-failure
cmake --preset asan && cmake --build --preset asan && ctest --preset asan --output-on-failure
cmake --preset tsan && cmake --build --preset tsan && ctest --preset tsan --output-on-failure
cmake --preset release && cmake --build --preset release && ctest --preset release --output-on-failure
cmake --preset dev -B build/dev-off -DCHATBOT_BUILD_TELEGRAM=OFF && cmake --build build/dev-off && ctest --test-dir build/dev-off --output-on-failure
```

Windows, terminal MSYS2 UCRT64:

```bash
pacman -S --needed git mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,curl,nlohmann-json,catch}
cmake --preset dev     && cmake --build --preset dev     && ctest --preset dev     --output-on-failure
cmake --preset release && cmake --build --preset release && ctest --preset release --output-on-failure
```

- Comprobar también el loop real de FTXUI con `./build/tsan/cli/chatbot`, conversación y cancelación manuales, sin `WARNING: ThreadSanitizer` (API real solo con permiso).
- CI: GCC en los cuatro presets, Clang dev/release, dev sin addons, Windows dev/release; **sin keys ni API real**. TSan: `vm.mmap_rnd_bits=28`.
- Latencia de cancelación **< 2 s en todos los presets**, incluido apagado y destructores. Las pruebas de tiempo siempre miden y reportan con `WARN`; los demás límites de rendimiento fallan solo con `CHATBOT_STRICT_PERF=1` (CI solo release). Las esperas funcionales entre hilos siempre son obligatorias.
- No desactivar comprobaciones de dependencias/includes ni permitir licencias ausentes en el ZIP.

Detalle: guias/11-compilacion.md

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

## 14. Addons

- Programas aparte en `addons/`, solo Linux de escritorio; Termux fuera de objetivo. Sin `.so` ni `dlopen`.
- Core y markdown no conocen addons. `cli/` solo usa `chatbot::addon_settings`, nunca bot ni telegram. `addon_settings`: core y nlohmann privado. Addons: core, markdown, addon_settings y bot, nunca `cli/` ni FTXUI. Lo compartido se extrae, no se copia.
- Settings siempre se compila, common con cualquier addon. Cada addon registra opción `CHATBOT_BUILD_<X>`, configuración versionada, entrada en `known_addons` y targets (incluidas pruebas) en `CHATBOT_ADDON_TARGETS`.
- Lanzar con `execv`: binario junto a la app, luego PATH sin carpeta actual; portable solo junto a la app. `--config-version` sin configuración ni red; incompatibilidad deja F2 en solo lectura.
- **No cambiar sin preguntar:** conversaciones solo en memoria con caducidad o `/nuevo`; acceso denegado por defecto, solo IDs permitidos o invitados por código; invitados guardados solo por ID; registros sin mensajes, consultas, nombres, código de invitación, token ni keys. Errores detallados solo con `log_error_details`, truncados y sin secretos.
- Una respuesta por chat; tope diario por usuario y fecha local; concurrencia global con FIFO. Búsqueda fallida/vacía devuelve cupo. Historial solo pares completos; cancelación no guarda el par. Apagado cancela, descarta fila y une hilos en < 2 s.
- Telegram: long polling, HTTPS de salida, chats privados, markdown saneado por lista blanca y respaldo plano partido a 4096; vista previa con Detener. Enteros de updates validados; token redactado en todo error, sin mostrar `redirect_url`.
- Solo TUI escribe configuración central, sin `telegram.json` ni `--config`. Bot solo lee entorno de secretos: token `CHAT_TELEGRAM_TOKEN` > `telegram`; modelo `CHAT_API_KEY` > `telegram:api_key` > app; búsqueda `CHAT_SEARCH_API_KEY` > `telegram:search_key` > `search:tavily`.
- Invitados: lectura/cambio/escritura bajo `flock`, atómica 0600, carpeta 0700; archivo dañado se conserva. SIGHUP inválido conserva configuración; token y concurrencia requieren reinicio; respuesta en curso conserva su runtime.
- Systemd: no reemplazar unidad ajena sin preguntar; nunca ejecutar enable-linger. Guardar aplica el estado Activado. Preguntas con `yes_no_answer`; systemd no reinicia salidas 65/69/73/78.

Detalle: guias/14-addons.md
