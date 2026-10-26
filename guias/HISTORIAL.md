# Guía: 3. Alcance por hitos
> Las reglas resumidas están en [AGENTS.md](../AGENTS.md).
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
| — | Pegar y caja de varias líneas: `\` + Enter y Alt+Enter, altura hasta 8 líneas, pegado entre corchetes (2004) en todas las plataformas, Ctrl+V / Ctrl+Shift+V / Shift+Insert con el portapapeles de Windows (`paste`, `input_edit`, `read_native_clipboard`) | En revisión |
| — | Menú de comandos al escribir `/`: registro único (`commands`), disponibilidad, `CommandMenu` (filtro, ↑/↓, Tab, Enter, Esc), vista entre la línea de estado y la caja, ratón, placeholder y README | En revisión |
| W2a | Windows, consola: símbolos `Automático / Unicode / Sencillos` (`glyphs`, `Palette::glyph`, `"symbols"` en `"appearance"`, `windows_build`), QuickEdit activo y ratón en conhost (verificación con `CHAT_DEBUG_KEYS`) | En revisión |
| W2b | Windows: modo portable (`chatbot.portable` junto al ejecutable → `datos/`, en todas las plataformas: `executable_dir`, `portable_data_dir`, `prepare_portable_data_dir`, línea en F2), pausa antes de cerrar la ventana del doble clic (`console_closes_on_exit`), ZIP de distribución (`tools/package-windows.sh`, `LEEME.txt`, `AVISOS-TERCEROS.txt`) en la CI; placeholder de la key de Tavily que cabe en su campo | En revisión |
| — | Ajuste de línea en los campos de varias líneas: modelo `TextEdit` (`text_edit`: glifos, distribución por palabras con columna reservada para el cursor, distribución incremental, cursor con afinidad, vista propia) y componente `TextArea` (`text_area`: `Node` con varias pasadas de layout, barra, cursor real, ratón); caja de la conversación (`input_max_rows`) y "Instrucciones del sistema" | En revisión |
| — | Pegado rápido: FTXUI v7.0.3 con el parche del #1351 de upstream (`cmake/patches/ftxui-7.0.3-drain-input.patch`, `cmake/apply_patch.cmake`), siempre por FetchContent; salida de emergencia del pegado medida en cuadros (`BracketedPaste::frame`, `RequestAnimationFrame` con un pegado abierto); pruebas con una pty (`chatbot_paste_probe`, `test_paste_pty.cpp`) | En revisión |
| A0 | Addons, base: `chatbot::markdown` (`markdown/`, con `source_blocks`), piezas puras de la app al núcleo (`chatbot/system_prompt.h`, `chatbot/search_context.h`, `complete_trimmed`), `addons/` en CMake (`CHATBOT_BUILD_TELEGRAM`, presets `dev-telegram` y `release-telegram`, CI, revisión de dependencias) y capa común de bots `chatbot::bot` (`addons/common/`), sin red | En revisión |
| A1 | Telegram (`feat/telegram`): cliente de la Bot API (`BotApi`, long polling con `Poller`, token tapado en todo error), mensajes enriquecidos con el markdown del modelo saneado (`to_rich_markdown`) y streaming con vista previa y "Detener" (`Draft`, `StreamingReply`, `Bot::stop`), respaldo en texto plano partido a 4096, configuración del bot (archivo JSON, entorno y `credentials.json`), invitados en `invitados.json`, tope diario por zona horaria (`day_in_zone`), mensajes viejos, ejecutable `chatbot-telegram`, sonda `chatbot-telegram-probe`, README y systemd | En revisión |
| A2 | Telegram como addon de la app (`feat/addon-integration`): `chatbot telegram` (addon estilo git, `addon_launcher`), configuración central (`config.json` → `addons.telegram` y `credentials.json` de la app) con las reglas en `chatbot::addon_settings` (`addons/settings/`), `--config-version`, códigos de salida de sysexits, recarga en caliente con SIGHUP (`bot::Runtime`, `Bot::reconfigure`, `reload_settings`, `SignalWatcher`), servicio de systemd de usuario escrito y controlado desde la TUI (`service_control`, `child_process`), invitados bajo `flock`, F2 → Addons (`addon_form`, `addons_panel`); sin `telegram.json`, `--config` ni los presets `dev-telegram`/`release-telegram` | En revisión |
| R0 | `Session` sin FTXUI (`refactor/session`): la lógica de `main.cpp` que no dibuja (configuración vigente, cliente y runner, búsqueda, la conversación abierta y su almacén, enviar y los comandos locales, abrir/nueva/borrar, guardados de F2 y sus formularios) pasa a `chatbot::cli::Session` (`session.h`), que comparten la TUI y el futuro `chatbot-rpc`; la TUI no cambia | En revisión |
| R1 | `chatbot-rpc` con chat (`feat/rpc-chat`): `rpc/` (`chatbot_rpc_lib` y `chatbot-rpc`), transporte NDJSON (tope de 1 MiB, stdout solo del protocolo, modo binario en Windows), ciclo de vida (`initialize`, `shutdown`, `exit`, EOF con reloj de seguridad de 3 s), errores de JSON-RPC, `$/cancel`, `app.status`, `conversation.new/get`, `chat.send` (con `search`) y `chat.cancel` con sus eventos (`diff_entries`, deltas agrupados en 30 ms), `CHAT_DEBUG_RPC`, `rpc/PROTOCOL.md`; en `Session`: `send`, `Finished`, `startup_warnings`, `has_search_key`; arranque común (`app_startup.h`); `tests/session_fakes.hpp` | En revisión |
| W3 | Windows: Windows Terminal portable en el ZIP, DPAPI, compilación cruzada desde Fedora y releases publicados | Fuera de alcance |

**Nota para el hito 4: prueba con hilos** (sin loop de FTXUI). Implementada con `RequestRunner` (`cli/src/request_runner.*`) y `tests/test_request_runner.cpp`; el transporte que se bloquea quedó aparte, en `tests/blocking_transport.hpp`:

- Extraer el hilo de trabajo de `cli/src/main.cpp` a una clase sin FTXUI que reciba una función para pasar tareas al hilo de la interfaz. En producción esa función hace `screen.Post` + `PostEvent(Event::Custom)`; en las pruebas, mete las tareas en una cola que la prueba vacía.
- Agregar a `FakeTransport` un bloqueo controlable para detener el flujo a la mitad.
- Correr la prueba con el preset `tsan`.

Todo lo de hitos posteriores está **fuera de alcance** del hito actual. No lo adelantes.

