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
| 3 | Interfaz FTXUI mínima (historial arriba, caja de entrada abajo) | **Actual** |
| 4 | Cancelación, scroll del historial, recorte de historial largo | Pendiente |
| 5 | Persistencia de conversaciones y render de markdown | Pendiente |

Todo lo de hitos posteriores está **fuera de alcance** del hito actual. No lo adelantes.

## 4. Stack

- **Lenguaje:** C++20. No usar funciones de C++23 (por ejemplo, `std::expected`).
- **Build:** CMake 3.21 o superior, con presets. Generador Ninja.
- **Compiladores:** GCC y Clang deben compilar sin warnings.
- **Dependencias permitidas:** libcurl, nlohmann/json 3.x, Catch2 v3 (solo pruebas), FTXUI ≥ 7.0.2 (solo `cli/`; fijada a v7.0.3 en FetchContent).
- Resolver dependencias con `find_package`; si no están instaladas, usar `FetchContent` con versión fija (nunca `master`/`main`).
- **Ninguna otra dependencia sin preguntar.**

## 5. Estructura de directorios

```
chatbot/
├── AGENTS.md
├── CMakeLists.txt
├── CMakePresets.json
├── .gitignore
├── core/
│   ├── CMakeLists.txt
│   ├── include/chatbot/     # headers públicos
│   └── src/
├── tools/                   # smoke.cpp: programa desechable para probar el núcleo
├── cli/
│   ├── CMakeLists.txt       # dependencia FTXUI, chatbot_cli_lib y ejecutable chatbot
│   └── src/
│       ├── conversation.h/.cpp  # lógica de la conversación (sin FTXUI ni hilos)
│       └── main.cpp             # interfaz FTXUI (ejecutable chatbot)
└── tests/
```

- Biblioteca estática `chatbot_core`, namespace `chatbot`.
- `core/` no puede depender de FTXUI ni leer/escribir en la terminal (salvo `tools/`).
- `chatbot_cli_lib` (biblioteca estática de `cli/`) solo depende de `chatbot::core`, sin FTXUI, para poder probarla. Solo el ejecutable `chatbot` enlaza FTXUI.

## 6. Diseño del núcleo

- **Tipos básicos:** `Role` (`System`, `User`, `Assistant`), `Message { Role role; std::string content; }`, `Config`.
- **Errores:** `ChatError { ErrorKind kind; int http_status; std::string message; std::optional<std::chrono::seconds> retry_after; }`.
  `ErrorKind`: `Config`, `Auth`, `ModelNotFound`, `RateLimited`, `Server`, `Network`, `Timeout`, `BadResponse`, `Cancelled`.
- **Resultado:** `Result<T>` mínimo sobre `std::variant<T, ChatError>`. Las excepciones pueden usarse internamente, **pero nunca cruzan la API pública**.
- **Transporte:** interfaz `Transport` que hace el POST y devuelve estado HTTP, cabeceras relevantes (`Retry-After`) y cuerpo, o un error de red. `CurlTransport` es la implementación real; las pruebas usan un `FakeTransport`. Diséñala para poder agregar streaming en el hito 2 sin romper `complete`; si hace falta cambiar algo, pregunta.
- **Cliente:** `ChatClient(Config, std::unique_ptr<Transport>)` con `complete(const std::vector<Message>&) -> Result<std::string>`.
- **Reintentos** (dentro de `ChatClient`):
  - Reintentar solo `RateLimited`, `Server`, `Network` y `Timeout`.
  - Máximo 3 reintentos, con espera exponencial desde 1 s. Si hay `Retry-After`, respetarlo.
  - Nunca reintentar `Auth`, `ModelNotFound`, `BadResponse` ni `Config`.
  - La espera se inyecta (interfaz `Sleeper` o similar) para que las pruebas no esperen de verdad.
- El historial lo administra quien llama; el núcleo no lo guarda en el hito 1.

## 7. Configuración

Precedencia: **variables de entorno > archivo de configuración > valores por defecto.**

| Dato | Variable | Archivo | Por defecto |
|------|----------|---------|-------------|
| API key | `CHAT_API_KEY` | **nunca** | ninguno (error `Config` si falta) |
| URL base | `CHAT_BASE_URL` | `base_url` | `https://integrate.api.nvidia.com/v1` |
| Modelo | `CHAT_MODEL` | `model` | **ninguno** (error `Config` con mensaje claro si falta) |
| Timeout (s): total en `complete`, por inactividad en `complete_stream` | `CHAT_TIMEOUT` | `timeout_seconds` | 120 |

- Archivo: `$XDG_CONFIG_HOME/chatbot/config.json`, o `~/.config/chatbot/config.json` si no existe esa variable. Que el archivo no exista no es un error.
- **Sin modelo por defecto a propósito:** los modelos de NIM se retiran con el tiempo y un nombre fijo en el código acabaría roto.
- La key **solo** viene del entorno.

## 8. HTTP y API

- Cuerpo de la petición: `model`, `messages` (cada uno con `role` y `content`), `stream: false` en el hito 1.
- Respuesta: leer `choices[0].message.content`. Validar que cada campo exista; si falta algo, devolver `BadResponse`, nunca tronar ni acceder a un índice inexistente.
- **Mapeo de estados HTTP:**
  - 2xx: éxito.
  - 401 y 403: `Auth`.
  - 404 y 410: `ModelNotFound`.
  - 429: `RateLimited`.
  - 5xx: `Server`.
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

Comandos de verificación:

```bash
cmake --preset dev  && cmake --build --preset dev  && ctest --preset dev  --output-on-failure
cmake --preset asan && cmake --build --preset asan && ctest --preset asan --output-on-failure
cmake --preset tsan && cmake --build --preset tsan && ctest --preset tsan --output-on-failure
```

Las pruebas de Catch2 no crean hilos: para cubrir los hilos de verdad, corre también `./build/tsan/cli/chatbot` con una conversación real y revisa que no aparezca `WARNING: ThreadSanitizer`.

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

<!-- antislop:start -->
## antislop
Para trabajo de UI, copys, accesibilidad, layout o comentarios de código, carga el núcleo (`antislop`) y la skill de la tarea con la herramienta `skill`:
- UI / visual: `antislop-ui`
- Copys y textos: `antislop-copywriting`
- Personas (contraste, teclado, foco, estados): `antislop-human`
- Móvil / responsive: `antislop-layoutmobile`
- Comentarios de código: `antislop-code`
Antes de empezar, pregunta al usuario cuándo aplica antislop: durante el trabajo, o después de terminado. Modo acordado para este proyecto: durante (1).
<!-- antislop:end -->
