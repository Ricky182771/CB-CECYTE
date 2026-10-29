# Guía: 11. Compilación y verificación
> Las reglas resumidas están en [AGENTS.md](../AGENTS.md).
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
- En Linux, los cuatro compilan también el addon de Telegram (`CHATBOT_BUILD_TELEGRAM` encendida por defecto, sección 14), también `asan` y `tsan` en `build/asan` y `build/tsan`. Sin addons: `cmake --preset dev -B build/dev-off -DCHATBOT_BUILD_TELEGRAM=OFF`. Los presets `dev-telegram` y `release-telegram` ya no existen. `option(... ON)` no cambia una caché existente: una carpeta configurada con el addon apagado se borra o se reconfigura una vez con `-DCHATBOT_BUILD_TELEGRAM=ON`.
- **Con poca memoria** (6 GB): `asan` compila con `cmake --build --preset asan -j2` y prueba con `ctest --preset asan -j1`; con más hilos el equipo se queda sin memoria.

`asan` y `tsan` ponen los sanitizadores también en `CMAKE_C_FLAGS`, para que md4c (C) quede instrumentado. Los flags de warnings solo se aplican a nuestros targets (`chatbot_set_warnings`), no a md4c ni a FTXUI.

Comandos de verificación:

```bash
cmake --preset dev  && cmake --build --preset dev  && ctest --preset dev  --output-on-failure
cmake --preset asan && cmake --build --preset asan && ctest --preset asan --output-on-failure
cmake --preset tsan && cmake --build --preset tsan && ctest --preset tsan --output-on-failure
cmake --preset release && cmake --build --preset release && ctest --preset release --output-on-failure
cmake --preset dev -B build/dev-off -DCHATBOT_BUILD_TELEGRAM=OFF && cmake --build build/dev-off && ctest --test-dir build/dev-off --output-on-failure
```

Las pruebas de `RequestRunner` y de `CancelToken` crean hilos (deben pasar con `tsan`). Aun así, corre también `./build/tsan/cli/chatbot` con una conversación real, cancelando un par de veces, y revisa que no aparezca `WARNING: ThreadSanitizer`: el loop de FTXUI no está en las pruebas.

La CI (`.github/workflows/ci.yml`, en cada PR y en cada push a `main`) corre estos mismos comandos con GCC en `dev`, `asan`, `tsan` y `release`, y con Clang en `dev` y `release`, todos con el addon (y revisa que `chatbot telegram` desactivado salga con 0 y sin token con 78); el trabajo `gcc / dev sin addons` compila y prueba `dev` con `-DCHATBOT_BUILD_TELEGRAM=OFF` y revisa que `chatbot telegram` salga con 69. Además, comprueba que el ejecutable sin `CHAT_API_KEY` salga con código 1 y que `chatbot-rpc` con `initialize`, `shutdown` y `exit` por stdin salga con 0 y diga `"ready":false`. **La CI nunca usa API keys ni llama a una API real.** En el trabajo `tsan` pone `vm.mmap_rnd_bits=28`, porque con 32 TSan falla al arrancar.

**Windows** (MSYS2 UCRT64, en la terminal "MSYS2 UCRT64"):

```bash
pacman -S --needed git mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,curl,nlohmann-json,catch}
cmake --preset dev     && cmake --build --preset dev     && ctest --preset dev     --output-on-failure
cmake --preset release && cmake --build --preset release && ctest --preset release --output-on-failure
```

El trabajo `windows` de la CI (`windows-2025`, `msys2/setup-msys2` con `UCRT64`) corre esos dos presets, sin `CHATBOT_STRICT_PERF` (los runners de Windows varían; la latencia de cancelación < 2 s se exige igual), no toca `core.autocrlf` (prueba lo que obtiene quien clona con Git for Windows; los saltos de línea los fija `.gitattributes`: LF en todo, `.ans` sin conversión porque llevan secuencias de escape, `.cmd`/`.bat` en CRLF para CMD), imprime `curl -V` (versión y backend TLS: el curl de MSYS2 UCRT64 usa OpenSSL), revisa el arranque sin key con stdin en `NUL` y `timeout`, y en release arma el ZIP con `tools/package-windows.sh` (falla si falta el texto de una licencia o si el ZIP no trae exactamente lo esperado; si un paquete no instala su licencia, la toma de `tools/windows-dist/licenses/<paquete sin prefijo>/` y avisa, sin fallar, si la versión instalada no es la de su `README.md`; nunca `--allow-missing-licenses` en la CI), lo descomprime, arranca `chatbot.exe` desde ahí sin `/ucrt64/bin` en el `PATH` (sin y con `chatbot.portable`) y sube `build/chatbot-windows-x64.zip` como artefacto `chatbot-windows-x64`, para probar en una PC sin MSYS2. Sin releases automáticos. Las pruebas de permisos POSIX (0600, 0700, umask, "chmod 600") y de enlaces simbólicos no se compilan en Windows; ahí corren las de Windows (credenciales sin `chmod`, rutas con `\`).

`ctest` corre `chatbot_addon_settings_tests` (`chatbot::addon_settings`; con `invitados.json` bajo `flock` con escrituras a la vez), `chatbot_tests` (núcleo y `chatbot_cli_lib`, sin FTXUI; los procesos de `service_control` con un `ProcessRunner` falso, salvo los de `run_process`, que lanzan `/bin/sh`, `sleep` y coreutils), `chatbot_markdown_tests` (solo `chatbot::markdown`, sin núcleo, `cli/` ni FTXUI) y `chatbot_ui_tests` (vista de markdown e historial y temas, dibujando con `ftxui::Screen`, sin terminal; F2 → Addons con un `ProcessRunner` falso y archivos en una carpeta temporal, también a 80×24); `chatbot_include_rules` (`tests/include_rules.cmake` con `cmake -P`, también con los addons apagados: fuera de `addons/` nadie incluye `chatbot/bot/` ni `chatbot/telegram/`, solo `cli/`, `rpc/`, `tools/` y `tests/` incluyen `chatbot/addon_settings/`, `addons/settings/` no incluye `chatbot/bot/` ni `chatbot/telegram/`, `markdown/` y `addons/` no incluyen FTXUI ni headers de `cli/src`, y `rpc/` no incluye FTXUI) y `chatbot_include_rules_check` (que falle con un `#include` prohibido); `chatbot_addon_launcher` (`tests/addon_launcher.cmake`: `chatbot <addon>` con el ejecutable real y un addon falso); `chatbot_rpc_tests` (puras, el `Dispatcher` con `session_fakes.hpp` y un reloj falso, y en POSIX `chatbot-rpc` real con pipes y un entorno limpio); con un addon encendido, `chatbot_bot_tests` y, con `CHATBOT_BUILD_TELEGRAM`, `chatbot_telegram_tests` (etiqueta `addons`; sin red: un Telegram falso y el modelo falso de `bot_harness.hpp`); y `tests/portable_startup.cmake` con el `chatbot` real (`chatbot_portable_writable` y, solo POSIX, `chatbot_portable_readonly`, que imprime `SKIP:` como root). En POSIX, `chatbot_ui_tests` también trae `test_paste_pty.cpp`: abre una pty (`posix_openpt`), arranca `chatbot_paste_probe` con la esclava como terminal de control, espera su primer cuadro (ya en modo crudo) y le escribe 200 KB entre corchetes mientras vacía la salida; exige que lleguen completos y en un solo pegado (`\r` → `\n`) y, con `CHATBOT_STRICT_PERF=1`, en menos de 2 s (sin el parche de FTXUI tarda 26 s, sea cual sea el equipo: 128 bytes por vuelta a 60 vueltas por segundo). Otra prueba abre un pegado sin `201~`, espera 3 × `kPasteIdleLimit` y manda una tecla: el pegado debe cerrarse (falla si la app deja de dibujar cuadros con el pegado abierto). `tests/data/markdown_muestra_terminal_w40.ans` y `_w80.ans` son `Screen::ToString()` del render con el tema "De la terminal" (24 bits); si cambia el render a propósito, se regeneran con la salida de `md_preview --theme terminal` en una terminal de 24 bits, quitando el `\r` extra que agrega la pty y el salto de línea final. Todas las pruebas de tiempo miden y reportan siempre con `WARN`. Las de latencia de cancelación (`CancelToken`, `RealSleeper`, `RequestRunner`, incluido su destructor, el apagado y el "Detener" de `chatbot::bot::Bot`, y en Telegram el corte del long polling, "Detener" desde un update y el apagado con varios chats; `report_latency` en `tests/performance.hpp`) exigen **< 2 s en todos los presets**, sin depender de `CHATBOT_STRICT_PERF`: miden la respuesta a la cancelación, no la velocidad del equipo. Los límites de las demás (parseo, reparto de tablas, historial) solo fallan con `CHATBOT_STRICT_PERF=1`, que la CI activa únicamente en release (GCC y Clang). Las esperas máximas entre hilos y la comprobación de no terminar una espera antes de tiempo son verificaciones funcionales y siguen siendo obligatorias. Para revisar el render a ojo: `./build/dev/tools/md_preview --width 40 tests/data/markdown_muestra.md`.

