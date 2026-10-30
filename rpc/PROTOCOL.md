# Protocolo de `chatbot-rpc`

Versión del protocolo: **1**.

`chatbot-rpc` es la app del chatbot sin interfaz: habla JSON-RPC 2.0 por stdin/stdout, una línea por mensaje, y usa la misma lógica que la TUI (`chatbot::cli::Session`). Sirve para la interfaz gráfica de Flutter y para que alguien adapte otra interfaz sin reescribir nada en otro lenguaje. Lo que cambie el protocolo se documenta aquí en el mismo PR.

## Estado

| Método o evento | Sección | Estado |
|---|---|---|
| `initialize`, `shutdown`, `exit`, `$/cancel` | 3, 4 | Implementado |
| `app.status` | 6.1 | Implementado |
| `conversations.list`, `conversation.delete` | 6.2 | Implementado |
| `conversation.new`, `conversation.get`, `conversation.open`, `conversation.export` | 6.3 | Implementado |
| `chat.send` (con y sin `search`), `chat.cancel` | 6.4 | Implementado |
| `commands.list`, `commands.run` | 6.5 | Implementado |
| `blocks.list`, `blocks.get`, `blocks.save` | 6.6 | Implementado |
| `settings.provider.*`, `models.list`, `settings.system_prompt.*`, `settings.search.*`, `settings.gui.*` | 6.7 | Implementado |
| `addons.*`, `service.*` | 6.8 | Falta (`-32601`) |
| Eventos `conversation.entry_added`, `entry_delta`, `entry_updated`, `phase`, `saved`, `closed`; `chat.finished`; `conversations.changed` | 7 | Implementado |
| Evento `settings.changed` (`provider`, `system_prompt`, `search`, `gui`) | 7 | Implementado |
| Evento `service.changed`; `settings.changed` con `telegram` | 7 | Falta |
| `CHAT_DEBUG_RPC` | 10 | Implementado |
| Windows: stdio en modo binario | 2 | Implementado, probado en la CI con pipes (`tools/check-rpc-pipes.sh`) |

---

## 1. Arquitectura

```
┌─────────────── Flutter (Dart) ───────────────┐
│  UI  ──►  RpcClient (json_rpc_2.Peer)        │
└───────────────┬───────────────▲──────────────┘
          stdin │ (una línea    │ stdout (una línea
                │  por mensaje) │  por mensaje)       stderr → registro
┌───────────────▼───────────────┴──────────────┐
│ chatbot-rpc                                  │
│  hilo lector ──► cola ──► hilo principal     │
│                  ▲        (despachador +     │
│  RequestRunner ──┤         Session +         │
│  ModelsLoader ───┤         escritor)         │
│  ServiceController┘                          │
│        chatbot_cli_lib + chatbot::core       │
└──────────────────────────────────────────────┘
```

- **Ubicación:** `rpc/`, con `chatbot_rpc_lib` (todo menos `main.cpp`) y el ejecutable `chatbot-rpc`, que enlazan `chatbot_cli_lib`. No pueden depender de FTXUI, del dibujo de `cli/` ni de los addons, ni transitivamente (`chatbot_forbid_ui_dependencies`, `chatbot_forbid_addon_dependencies` y la revisión de `#include`).
- **Hilos:**
  - **Lector:** lee stdin línea por línea y mete cada línea en la cola del hilo principal. Al llegar EOF mete un evento `Eof`.
  - **Principal:** saca de la cola, despacha, llama a `Session` y escribe todas las respuestas y eventos. Como todo lo que se escribe sale de este hilo, nunca se mezclan dos mensajes en una línea y el orden está garantizado.
  - **Trabajadores:** los que ya existen. Su `Post` pasa a ser "meter la tarea en la cola del hilo principal", igual que en las pruebas de `RequestRunner`. No hace falta tocarlos.
- **`Session`:** dueña de la configuración efectiva, `ChatClient`, `RequestRunner`, `ConversationStore`, `SearchProvider` y la conversación abierta; la usan la TUI y el RPC. El `Dispatcher` del RPC le pasa las peticiones y, después de cada petición y de cada tarea de un hilo de trabajo, compara la conversación con lo que ya emitió (`diff_entries`, sección 5) y escribe los eventos.

---

## 2. Transporte

1. **JSON-RPC 2.0**, sin lotes (un arreglo de nivel superior → error `-32600`).
2. **Un mensaje por línea:** UTF-8, terminado en `\n` (sin `\r`). Los saltos de línea dentro de cadenas van escapados (`\n`), así que un mensaje nunca ocupa dos líneas.
3. **Tamaño máximo de línea:** 1 MiB. Si una línea pasa del tope, el servidor descarta hasta el siguiente `\n` y responde `-32600` con `id: null`. Cubre de sobra el tope de pegado de la TUI (256 KiB) y el del system prompt (8000 bytes). Al leer se tolera `\r\n`, y las líneas vacías se ignoran.
4. **stdout es solo del protocolo.** Al arrancar, el servidor duplica el descriptor 1 para su escritor y apunta el 1 a stderr (`dup`/`dup2`; en Windows `_dup`/`_dup2`). Así, si una biblioteca imprime algo por accidente, sale en stderr y no rompe el protocolo.
5. **stderr:** registro libre para humanos. El cliente lo guarda en un log y nunca lo interpreta.
6. **Windows:** stdin y stdout en modo binario (`_setmode(..., _O_BINARY)`) para que el CRT no convierta `\n` en `\r\n` (`rpc/src/stdio_windows.cpp`; en POSIX, `stdio_posix.cpp`, que además no deja que los procesos hijos hereden la copia del descriptor).
7. **Salida JSON:** `nlohmann::json::dump(-1, ' ', false, error_handler_t::replace)`. Si algo trae UTF-8 inválido, se sustituye en lugar de lanzar. Se hace `flush` después de cada mensaje.
8. **SIGPIPE ignorada** en POSIX: si el cliente murió, la escritura falla y el servidor sale.
9. **Sin sockets.** Nada de TCP ni HTTP local. Con stdio, solo el proceso que lanzó al servidor puede hablarle, así que no hace falta autenticación.

### Windows

- `chatbot-rpc.exe` es un **programa de consola** (sin `-mwindows`): desde CMD se ve su stderr. Viene en el ZIP junto a `chatbot.exe`.
- El cliente lo crea **sin ventana**. Dart con `Process.start` en modo normal (con pipes) ya lo hace: usa `CREATE_NO_WINDOW` y solo la quita con `ProcessStartMode.inheritStdio`. Otros clientes deben pasar `CREATE_NO_WINDOW` a `CreateProcessW`.
- stdin y stdout en modo binario: cada mensaje termina en `\n`, sin `\r`, y es UTF-8 (sin páginas de códigos de por medio). Una entrada con `\r\n` también se acepta.
- No se lanza desde `cmd /c` ni con `runInShell: true`: la shell de por medio cambia el manejo de comillas y de la consola, y al matarla queda vivo el servidor.
- Si stdin es una consola (doble clic en el `.exe`, o abierto desde CMD sin pipes), escribe en stderr, una sola vez, que es para otras interfaces y que para platicar se abre `chatbot`, y sigue funcionando igual. En Linux, lo mismo cuando stdin es una terminal.

---

## 3. Ciclo de vida

```
cliente                                  servidor
  │── initialize ─────────────────────────►│
  │◄──────────────────────── result ───────│  (versión, plataforma, rutas, estado)
  │── ...peticiones / eventos... ──────────│
  │── shutdown ───────────────────────────►│  cancela y espera hilos
  │◄──────────────────────── result ───────│
  │── exit (notificación) ────────────────►│  sale con 0
```

- Antes de `initialize`, cualquier otra petición recibe `-32001 NotInitialized`, salvo `shutdown`, `exit` y `$/cancel`. Un segundo `initialize` recibe `-32600`.
- Después de `shutdown`, todo lo que no sea `exit` recibe `-32600`.
- `shutdown` (después de un `initialize` que funcionó) **siempre** manda `conversation.closed` (`reason: "shutdown"`) a la conversación abierta antes del resultado, haya o no una respuesta en curso. Si la hay, la cancela y espera al hilo. **Esa respuesta no manda `chat.finished`:** es la única excepción a "siempre un `chat.finished` por `chat.send`", porque el cliente se está yendo.
- Un `models.list` pendiente al llegar `shutdown` o EOF se cancela y **no** se responde, igual que el `chat.finished` de arriba.
- **EOF en stdin** (la GUI se cerró o se cayó): el servidor cancela lo que esté en curso, espera a los hilos y sale con 0. Si en **3 s** no ha terminado, sale con `_exit(0)`. La cancelación de curl ya mide menos de 2 s en todos los presets. Esto evita procesos huérfanos en Linux y en Windows: al morir el padre, el pipe se cierra.
- `exit` sin `shutdown` previo: sale igual, con código 1. Si `exit` llega con `id`, primero responde `{}`.
- Dos GUIs abiertas son dos procesos con la misma carpeta de conversaciones, igual que dos TUIs. No se agrega nada nuevo para ese caso.
- **Limitación conocida:** si la TUI y la GUI (o dos de cualquiera) están abiertas y las dos guardan configuración, gana la última escritura: `config.json` se reescribe completo y sin candado. Es igual que con dos TUIs.

### `initialize`

```jsonc
// →
{"jsonrpc":"2.0","id":1,"method":"initialize","params":{
  "protocol_version":1,
  "client":{"name":"chatbot-gui","version":"0.1.0"}}}
// ←
{"jsonrpc":"2.0","id":1,"result":{
  "protocol_version":1,
  "server":{"name":"chatbot-rpc","version":"0.x"},
  "platform":"linux",                 // "linux" | "windows"
  "portable":false,
  "conversation":"c1",                // handle de la conversación abierta (siempre hay una)
  "paths":{"config":"/home/r/.config/chatbot/config.json",
           "conversations":"/home/r/.local/share/chatbot/conversations",
           "downloads":"/home/r/Descargas/chatbot"},   // null si no se pudo resolver
  "ready":false,                      // hay proveedor, key (o servidor local) y modelo
  "problem":"Configura un proveedor para empezar.",    // null si ready
  "warnings":["Las instrucciones del sistema no son texto; se usan las predeterminadas."],
  "capabilities":{"search":true,"addons":[],"service":null}}}
```

- Si `protocol_version` no coincide, el servidor responde `-32010 ProtocolMismatch` con `data: {"server": 1}` y el cliente avisa "actualiza la app".
- `ready: false` es el equivalente de la TUI abriendo F2 con `Configura un proveedor para empezar.`. La carga usa `allow_missing_key_and_model = true`, como la TUI.
- `warnings` son los avisos de arranque que la TUI manda a la línea de estado, por ejemplo un system prompt que no es cadena. Los avisos de tema y símbolos son solo de la TUI y no aparecen aquí.
- `capabilities` dice qué soporta este servidor y no cambia durante la sesión. `search: true`: `chat.send` acepta `search` (que haya key o no lo dice `app.status.has_search_key`). `addons` y `service` quedan en `[]` y `null` hasta que existan sus métodos (sección 6.8); en Windows se quedan así.
- `paths.downloads` es la carpeta de `/guardar` y `/exportar`, con su `chatbot/` (no se crea al arrancar).

---

## 4. Convenciones

- **Métodos:** `area.verbo` en inglés y snake_case (`conversation.open`, `settings.provider.save`). **Eventos:** notificaciones del servidor con el mismo estilo (`conversation.entry_delta`).
- **Campos:** snake_case.
- **Textos para el usuario** (`message`, `note`, `problem`): en español, los mismos de la TUI. La GUI los muestra tal cual, sin traducirlos ni armarlos.
- **Fechas:** ISO 8601 con desfase, como en los archivos (`2026-10-25T18:52:00-06:00`).
- **Enteros de 64 bits** (IDs de Telegram): como **cadena decimal**. JSON no garantiza enteros de más de 2^53 entre implementaciones.
- **Secretos** (`api_key`, `token`, `model_key`, `search_key`): solo viajan de la GUI al servidor. El servidor **nunca** los devuelve: regresa el estado enmascarado de `mask_key` (`guardada: …abcd`).
- **Parámetros desconocidos:** el servidor los rechaza con `-32602` y la lista de llaves. La GUI y el servidor se distribuyen juntos, así que esto atrapa errores de dedo. **Campos desconocidos en resultados y eventos:** el cliente los ignora, para que agregar campos no rompa nada.
- **`conversation`:** *handle* que asigna el servidor (`"c1"`, `"c2"`, ...). Una conversación nueva no tiene id guardado hasta su primer par terminado, así que el handle y el `id` del archivo son cosas distintas.

### Errores

Los códigos estándar de JSON-RPC más los de la aplicación, en el rango que JSON-RPC reserva para el servidor:

| Código | Nombre | Cuándo |
|---|---|---|
| -32700 | ParseError | La línea no es JSON |
| -32600 | InvalidRequest | No es una petición válida, es un lote o pasa de 1 MiB |
| -32601 | MethodNotFound | Método desconocido |
| -32602 | InvalidParams | Faltan parámetros, sobran o tienen otro tipo (`data.fields`) |
| -32603 | InternalError | Excepción atrapada en el despachador (no debería pasar) |
| -32800 | RequestCancelled | La petición se canceló con `$/cancel`, o otro `models.list` la reemplazó |
| -32001 | NotInitialized | Falta `initialize` |
| -32002 | Busy | Hay una respuesta en curso y la operación la necesita libre |
| -32003 | UnknownConversation | El handle no existe o ya se cerró |
| -32004 | NotConfigured | `chat.send` sin proveedor ni modelo |
| -32005 | ValidationFailed | No pasa la validación: configuración, texto vacío, número de bloque o nombre de archivo (`data.message` o `data.fields`) |
| -32006 | Unsupported | No disponible en esta plataforma (addons en Windows) |
| -32007 | StorageError | No se pudo leer, escribir o borrar un archivo, o no hay carpeta de descargas (`data.message`) |
| -32008 | ConfirmationRequired | Hay que preguntarle al usuario (unidad de systemd ajena; `data.message`) |
| -32010 | ProtocolMismatch | Versión distinta en `initialize` |
| -32011 | StartupFailed | El arranque falló (carpeta del modo portable no escribible, `config.json` inválido): `initialize` responde esto con `data.message`, el mismo texto que la TUI imprime al salir |

Los errores **del modelo** (401, 429, red, ...) **no** son errores de JSON-RPC. `chat.send` se acepta al instante y el resultado llega en el evento `chat.finished`, con un objeto `ChatError`:

```jsonc
{"kind":"rate_limited",          // config | auth | model_not_found | rate_limited | server |
                                 // network | timeout | bad_response | invalid_request |
                                 // cancelled | no_search_results
 "http_status":429,              // 0 si no hubo respuesta HTTP
 "message":"...",            // el mensaje del error, sin la etiqueta del tipo
 "retry_after_seconds":20}       // null si no vino
```

El texto que la TUI muestra (`[error de autenticación] La key de búsqueda no es válida`) llega en la entrada `error` de la conversación; `message` es solo el mensaje, porque el tipo ya va en `kind`.

### Cancelar peticiones largas

`$/cancel` es una notificación con el id de la petición: `{"method":"$/cancel","params":{"id":7}}`. Si es el de un `models.list` pendiente, ese `models.list` responde `-32800` en ese momento y su resultado se descarta; con cualquier otro id no hace nada. `$/cancel` nunca recibe respuesta propia como notificación; si llega con `id`, responde `{}` (después del `-32800`). Con R4 también servirá para `service.*` y `addons.check_install`. Para cancelar una respuesta del modelo se usa `chat.cancel`, porque `chat.send` no es una petición larga.

### Notificaciones

Un mensaje sin `id` es una notificación: se ejecuta, pero nunca recibe respuesta, ni siquiera de error (un método desconocido o parámetros inválidos se ignoran en silencio). Un mensaje inválido (sin `jsonrpc: "2.0"`, sin `method` cadena, o con un `id` que no es entero ni cadena) sí recibe `-32600`, con su `id` si era válido y si no con `id: null`.

---

## 5. Modelo de la conversación

La GUI mantiene una **copia exacta** de `Conversation::entries()`. Las entradas solo se agregan o se modifican, nunca se borran ni cambian de lugar (hoy es así: `finish_error` marca la última y agrega otra; abrir otra conversación crea otro objeto). Por eso el **índice** de la entrada sirve para referirse a ella.

```jsonc
// Entry
{"kind":"assistant",        // user | assistant | error | notice | sources
 "text":"...",               // markdown crudo; la GUI lo dibuja (gpt_markdown)
 "in_progress":false,
 "incomplete":false,
 "cancelled":false,
 "note":"...",                // nota por finish_reason; "" si no hay
 "sources":[{"n":1,"title":"...","url":"https://...","published_date":"2026-10-20"}]}
                             // solo en kind = sources; published_date puede ser null
```

```jsonc
// ConversationSnapshot
{"conversation":"c1",
 "rev":42,                   // sube de uno en uno con cada cambio de esta conversación
 "id":"20261025-185200-a1b2c3",   // null si aún no se guarda
 "title":"Cómo compilar en Windows",   // "" si aún no se guarda
 "created_at":"...", "last_model":"...",
 "phase":"idle",             // idle | searching | thinking | streaming
 "entries":[Entry, ...]}
```

- **`phase`** reemplaza los indicadores de la línea de estado: `searching` es "Buscando en la web…", `thinking` es "Pensando…" (antes del primer texto) y `streaming` es cuando ya llega texto.
- **`rev`:** cada evento de una conversación lleva el `rev` que deja. Como el servidor escribe todo desde un hilo, el orden ya está garantizado. `rev` sirve para que la GUI compruebe que no se saltó nada (si llega `rev` ≠ el suyo + 1, pide `conversation.get`) y para descartar eventos viejos después de un snapshot.
- **Cómo se calculan los eventos:** función pura `diff_entries(visto, entradas)` (`rpc/src/entry_diff.h`) que da qué entradas se agregaron, si a la última ya vista se le agregó texto y si cambiaron sus marcas. Lo visto es solo cuántas entradas había y la última (largo y marcas), así que el costo por delta no crece con la conversación. Supone lo que hoy cumple `Conversation`: las entradas solo se agregan, de las existentes solo cambia la última, y su texto solo crece por el final.

---

## 6. Métodos

Notación: `→` parámetros, `←` resultado. `?` = opcional.

### 6.1 Aplicación

**`app.status`** `→ {}` `← {ready, problem, busy, conversation, env_locks, has_search_key}`
- `conversation`: handle de la conversación abierta.
- `env_locks`: las variables `CHAT_*` que fijan algo de la configuración y están definidas, por nombre (`["CHAT_MODEL"]`), nunca su valor: `CHAT_API_KEY`, `CHAT_BASE_URL`, `CHAT_MODEL`, `CHAT_TIMEOUT`, `CHAT_HISTORY_LIMIT`, `CHAT_SEARCH_API_KEY`, `CHAT_DATA_DIR` y `CHAT_DOWNLOAD_DIR`.
- `has_search_key`: hay key de búsqueda (`CHAT_SEARCH_API_KEY` o `credentials.json`); sin ella, `chat.send` con `search` responde `-32005`.

**`shutdown`** `→ {}` `← {}` · **`exit`** (notificación)

### 6.2 Lista de conversaciones (la barra)

**`conversations.list`** `→ {}` `← {items: [Summary]}`

```jsonc
// Summary (de ConversationSummary + group_label con el día local de hoy)
{"id":"...", "title":"...", "updated_at":"...", "message_count":6,
 "last_model":"...", "readable":true, "error":"",
 "group":"Hoy",                       // "Hoy" | "Ayer" | "Últimos 7 días" | "2 oct" | "Sin fecha"
 "open_as":"c1"}                      // handle si está abierta en este proceso, o null
```

Mismo orden que el almacén: la más reciente primero y las ilegibles al final. El grupo lo calcula el servidor con la misma regla que la barra de la TUI (`group_for`, en `cli/src/sidebar.h`), con el día local de la hora de la sesión: `Sin fecha` para las ilegibles o sin fecha válida.

**`conversation.delete`** `→ {id}` `← {replaced_by: ConversationSnapshot | null}`
- No pregunta. La confirmación (`¿Borrar …? (s/n)` en la TUI) es trabajo de la GUI.
- Si es la abierta, el servidor empieza una nueva, igual que la TUI con `new_conversation()`: el handle viejo recibe `conversation.closed` (`reason: "deleted"`) antes de la respuesta y `replaced_by` trae la nueva. Si no era la abierta, `replaced_by: null`.
- Después de la respuesta emite `conversations.changed`.
- Errores: `-32002 Busy` (`data.message`: `Espera la respuesta o cancélala con Esc.`) con **cualquier** respuesta en curso, como la TUI; `-32007` con `data.message` si es ilegible (nunca se borra), no existe o falló el borrado.

### 6.3 Conversación abierta

Siempre hay exactamente una conversación abierta, igual que en la TUI: al arrancar es una nueva y vacía, y `initialize` y `app.status` dan su handle. No hay `conversation.close`: `new` y `open` la reemplazan.

**`conversation.new`** `→ {}` `← ConversationSnapshot`
- Conversación vacía con las instrucciones del sistema vigentes. **Reemplaza** a la abierta: el handle anterior recibe `conversation.closed` (`reason: "replaced"`) antes de la respuesta, y desde ahí responde `-32003`. Con una respuesta en curso, `-32002 Busy` con `data.message` (`Espera la respuesta o cancélala con Esc.`), igual que Ctrl+N en la TUI.

**`conversation.open`** `→ {id}` `← ConversationSnapshot`
- Usa `ConversationStore::load` y `Conversation::from_stored`, con el mismo aviso de la TUI si cambió el modelo (`Esta conversación usó X; se continúa con Y.`). Ese aviso llega como entrada `notice` dentro del snapshot.
- Reemplaza a la abierta, igual que `conversation.new`: el handle anterior recibe `conversation.closed` (`reason: "replaced"`) antes de la respuesta. Si `id` ya es el de la abierta, devuelve el snapshot con el mismo handle, sin cerrar ni volver a cargar.
- Errores: `-32002 Busy` (`kWaitOrCancel`) con una respuesta en curso; `-32007` con el mensaje de `ConversationStore::load` si es ilegible o no existe (la abierta no cambia).

**`conversation.get`** `→ {conversation}` `← ConversationSnapshot`

**`conversation.export`** `→ {conversation}` `← {path, message}`
- Es el botón `[Exportar]`: `export_conversation` hacia la carpeta de descargas, **sin tocar el historial** y también con una respuesta en curso (exporta los pares ya guardados). `path` es la ruta completa del archivo; `message` es el aviso largo (`Conversación exportada a ~/…`).
- Errores: `-32005` sin pares (`Todavía no hay respuestas que exportar.`); `-32007` sin carpeta de descargas o si falla la escritura, con `data.message`.

### 6.4 Chat

**`chat.send`** `→ {conversation, text, search?}` `← {accepted: true}`
- `text` se manda tal cual: **no** se interpretan comandos (para eso está `commands.run`).
- `search` (cadena) hace lo mismo que `/buscar <consulta>`: `text` es lo que se muestra y se guarda, y `search` es la consulta. Para que se vea igual que en la TUI, la GUI manda `text: "/buscar <consulta>"`.
- Errores inmediatos, en este orden: `-32004 NotConfigured` (`data.message`: `Configura un proveedor para empezar.`), `-32002 Busy` (respuesta en curso), `-32005` con `data.message` `El mensaje está vacío.` si el texto queda vacío al recortarlo o `search` solo tiene espacios, y `-32005` con el texto de la TUI si falta la key de búsqueda (`Configura la key de búsqueda en Configuración (F2) → Búsqueda web`).
- Si se acepta, siguen los eventos de la sección 7 y siempre termina con un solo `chat.finished`.

**`chat.cancel`** `→ {conversation}` `← {cancelled: bool}`
- `false` si no había nada en curso. Es igual que Esc: búsqueda o respuesta. El resultado llega después en `chat.finished` (`outcome: "cancelled"`); lo que ya estaba en camino puede llegar antes como un último `entry_delta`.

### 6.5 Comandos (paridad con la caja de la TUI)

**`commands.list`** `→ {conversation}` `← {commands: [...]}`

```jsonc
{"name":"/buscar","arguments":"<consulta>",
 "description":"Busca en la web y responde con fuentes citadas",
 "needs_argument":true,
 "available":false, "note":"falta la key de búsqueda (F2)"}   // note: "" si available
```

Sale de `kCommands` y `command_availability`, así que la GUI puede tener su menú `/` sin repetir la lista. Funciona sin configuración, como el menú de la TUI.

**`commands.run`** `→ {conversation, text}` `← CommandOutcome`
- Hace exactamente lo de Enter en la TUI (`Session::submit`, con `parse_command`). Toda respuesta trae `kind` y `clear`: si la GUI vacía su caja, igual que la TUI.

```jsonc
// CommandOutcome
{"kind":"message", "clear":true}                     // texto normal enviado (como chat.send)
{"kind":"search", "clear":true}                      // /buscar <consulta> enviado
{"kind":"copy", "number":3, "language":"cpp", "text":"...código...",
 "message":"Copiado el bloque #3 (cpp, 24 líneas).", "clear":true}
{"kind":"save", "path":"/home/r/Descargas/chatbot/x.cpp", "message":"Guardado en ~/Descargas/chatbot/x.cpp",
 "error":false, "clear":true}                        // path null y error true si falló la escritura
{"kind":"export", "path":"...", "message":"Conversación exportada a ...", "error":false, "clear":true}
{"kind":"status", "message":"Uso: /copiar [número de bloque]", "clear":false}
```

- **Texto para el modelo** (`message`, `search`): si no se envía, los mismos errores de JSON-RPC que `chat.send`, en el mismo orden: `-32004`, `-32002`, `-32005` vacío (`El mensaje está vacío.`; con `/buscar` sin consulta, el uso de la TUI: `Uso: /buscar <consulta>`) y `-32005` sin key de búsqueda. Si se envía, siguen los eventos de la sección 7 y un `chat.finished`.
- **`status`:** lo que la TUI pone en la línea de estado dejando el texto en la caja, con el mismo texto: los usos (`/copiar x`), `Todavía no hay respuestas que exportar.`, un número de bloque que no existe, un nombre inválido, sin carpeta de descargas, `Espera a que termine la respuesta` (comandos locales con una respuesta en curso), etc. La GUI ya tiene el texto, así que no vuelve.
- **`copy`:** el servidor **no** toca el portapapeles: la GUI copia `text` (con `Clipboard.setData`). Como la GUI siempre puede copiar, el aviso (`message`, con el de `incomplete_warning` si el bloque está incompleto) **se agrega al historial** como en la TUI, y llega como `conversation.entry_added` (`notice`) después de la respuesta.
- **`save` y `export`:** como en la TUI, el resultado ya está en el historial (`entry_added` con el aviso, o con el error si falló la escritura). `path` es la ruta completa del archivo, o `null` si no se escribió; `message` lleva la ruta abreviada con `~`.

### 6.6 Bloques de código

**`blocks.list`** `→ {conversation}` `← {blocks: [...]}`

```jsonc
{"number":3, "entry":5, "language":"cpp", "lines":24,
 "complete":false, "reason":"in_progress"}   // none | cancelled | error | in_progress (IncompleteReason)
```

La numeración es la de `collect_code_blocks`, la misma `#3 · cpp` de la TUI. La GUI pone sus botones [Copiar] y [Guardar] por número.

**`blocks.get`** `→ {conversation, number}` `← {code, language, complete, reason}`
- Para el botón [Copiar] de la GUI. No toca el historial.
- Errores: `-32005` con el texto de `choose_code_block` si el número no existe.

**`blocks.save`** `→ {conversation, number, file_name?}` `← {path, message}`
- Es el botón `[Guardar]`: `save_block` con `validate_file_name`, **sin tocar el historial** y también con una respuesta en curso (un bloque incompleto lleva su aviso en `message`). Sin `file_name` usa `bloque-<n>.<ext>`. Nunca sobrescribe. `path` es la ruta completa; `message` es el aviso largo, con la ruta abreviada.
- Errores: `-32005` (número que no existe o nombre inválido, con el texto de la TUI); `-32007` (sin carpeta de descargas o error al escribir). Un `number` que no cabe en un entero de 32 bits es `-32602`.

### 6.7 Configuración (F2)

Todo es **sin estado**: la GUI tiene su formulario y manda el borrador completo. Por dentro, el servidor arma un `ProviderSettings` (o `SearchSettings`) nuevo por llamada con `Session::provider_form` (o `search_form`), le aplica el borrador con sus setters y usa su `validate`; guarda con los mismos métodos de `Session` que F2. Así las reglas no se copian a Dart.

**Guardar proveedor, instrucciones o búsqueda no se permite con una respuesta en curso** (`-32002` con `kWaitOrCancel`), igual que F2, que no abre en ese caso. Las preferencias de la GUI sí, porque no tocan el chat. Al guardar, el servidor **aplica sin reiniciar**, como la TUI. **Orden:** la respuesta, `settings.changed {section}` y después los avisos que el guardado dejó en el historial (`conversation.entry_added`, `notice`), los mismos de F2.

Si `config.json` o `credentials.json` no se pueden leer (JSON inválido, permisos abiertos), los `get`, `check`, `save` y `models.list` responden `-32007` con el mensaje.

#### Proveedor de IA

**`settings.provider.get`** `→ {}` `←`

```jsonc
{"providers":[{"id":"nvidia","name":"NVIDIA NIM",
               "base_url":"https://integrate.api.nvidia.com/v1","needs_key":true}, ...,
              {"id":"custom","name":"Personalizado…","base_url":"","needs_key":true}],
 "saved":{"provider":"nvidia","base_url":"...","model":"..."},   // config.json tal cual
 "effective":{"base_url":"...","model":"..."},                   // lo que usa la sesión
 "locks":{"base_url":false,"model":false,"api_key":true},   // definidos por CHAT_*
 "key_status":"(definido por CHAT_API_KEY) …abcd"}             // key_status(), enmascarado
```

`providers` es la tabla de `providers()`, en orden. Nunca devuelve una key.

**El borrador** (`draft`) de `check`, `save` y `models.list`:

```jsonc
{"provider":"custom", "base_url":"https://mi-servidor", "model":"...", "api_key":"nvapi-…"?}
```

- Se aplica, en este orden, a un `ProviderSettings` nuevo: `select_provider(provider_index(provider))`, `set_base_url` (solo cambia algo en `custom`), `set_key` (si viene `api_key`) y `set_model`.
- **Un campo fijado por `CHAT_*` que venga en el borrador se ignora**, igual que en F2, donde no se puede editar (la GUI lo sabe por `locks`). La `base_url` de un proveedor que no es `custom` también se ignora: se usa la de la tabla.
- Sin `api_key` (o con `""`) se usa la guardada, igual que el campo vacío en F2.
- `provider`, `base_url` y `model` son obligatorios. Un `provider` que no está en la tabla es `-32602` con `fields: ["draft.provider"]`: al leer `config.json` un id desconocido cae en `custom`, pero aquí es un error de la GUI. Los campos de más, de menos o de otro tipo también van como `draft.<campo>`.

**`settings.provider.check`** `→ {draft}` `← {error, hint, can_request_models, dirty}`

```jsonc
{"error":null,                // validate(); null si se puede guardar
 "hint":"...",                // base_url_hint(); null si no hay
 "can_request_models":true, "dirty":true}
```

**`settings.provider.save`** `→ {draft}` `← {ready}`
- `Session::save_provider`: escribe `credentials.json` (si hubo key) y `config.json`, y reconstruye `ChatClient` y `RequestRunner`. La conversación abierta no cambia y recibe el aviso `Ahora se usa … de ….`
- `ready`: si quedó configurado (el `ready` de `initialize`).
- Errores, en este orden: `-32602` (borrador), `-32002` (respuesta en curso), `-32005` con el error de `validate()` en `data.message`, `-32007` si falla la escritura.

**`models.list`** `→ {draft}` `← {models: ["..."]}` · *petición larga, cancelable*
- `ModelsLoader` con `request_config` del borrador sobre la configuración vigente. Es la única petición cuya respuesta sale después, por un hilo de trabajo; todas las demás responden al momento.
- **Toda petición recibe exactamente una respuesta:** la lista; `-32005` con el texto de F2 (`[error de red] …`, `models_error_text`, sin la key) si el servidor falla, para que la GUI deje escribir el modelo a mano; o `-32800` si se canceló.
- `-32005` con `Escribe una URL base válida y la API key para pedir los modelos.` si `can_request_models` es false, sin pedir nada (no cancela uno pendiente).
- Una sola a la vez: otro `models.list` (que sí se pide) reemplaza al pendiente, que responde `-32800` en ese momento. Si el usuario cambia de proveedor, la GUI puede mandar otro directamente o `$/cancel` del anterior.
- `shutdown` o EOF con uno pendiente: se cancela y no se responde (sección 3).
- El filtro y el resaltado son de la GUI (`filtered_models` es trivial). El servidor da la lista completa, ordenada.

#### Instrucciones del sistema

**`settings.system_prompt.get`** `→ {}` `←`

```jsonc
{"mode":"custom",              // default (llave ausente) | none ("") | custom (otro texto)
 "text":"Eres un tutor...",     // el texto vigente ("" con none)
 "default_text":"Eres un asistente útil. Responde en español, de forma clara y concisa.",
 "max_bytes":8000,
 "warning":null}                // system_prompt_limit_warning con el history_limit vigente
```

**`settings.system_prompt.check`** `→ {text}` `← {error, warning, bytes}`
- `error` de `validate_system_prompt` y `warning` de `system_prompt_limit_warning`, los mismos textos que F2 muestra debajo del campo; `bytes` en UTF-8.

**`settings.system_prompt.save`** `→ {text}` `← {mode}`
- `text` es siempre una cadena (**sin `null`**). `""` = sin mensaje de sistema. **Restaurar** = mandar `default_text`: `system_prompt_to_store` convierte el predeterminado en "borrar la llave", igual que F2.
- `Session::save_system_prompt_text`. Se aplica a la conversación abierta con `set_system_prompt`, desde su siguiente mensaje, con los avisos de F2 en el historial.
- Errores: `-32002` (respuesta en curso), `-32005` con el texto de `validate_system_prompt`, `-32007` si falla la escritura.

#### Búsqueda web

**`settings.search.get`** `→ {}` `← {configured, locked, key_status}`
- `configured`: hay key vigente (el `has_search_key` de `app.status`). `locked`: la fija `CHAT_SEARCH_API_KEY`. `key_status`: el de `SearchSettings`, enmascarado.

**`settings.search.save`** `→ {api_key}` `← {saved: true}`
- `Session::save_search_key`: `credentials.json` con `search:tavily` y reemplaza el `SearchProvider` (el runner ya acepta el cambio en caliente). Desde ahí `app.status.has_search_key` es `true`. El aviso de F2 va al historial.
- Errores, en este orden: `-32002` (respuesta en curso); `-32005` si `CHAT_SEARCH_API_KEY` la fija (`La key de búsqueda está definida por CHAT_SEARCH_API_KEY; no se puede cambiar aquí.`), si `validate()` la rechaza (el texto de F2) o si viene vacía (`Escribe la key de búsqueda.`); `-32007` si falla la escritura.

#### Preferencias de la GUI

La GUI necesita guardar sus propias cosas (tema claro u oscuro, ancho de la barra, tamaño de letra) y deben seguir el modo portable. Se guardan en `config.json` bajo la llave **`"gui"`**, con `load_config_section` y `save_config_section` (`Session::gui_settings` y `save_gui_settings`).

**`settings.gui.get`** `→ {}` `← {value: object | null}`
- `null` si la llave no existe. El servidor no lo interpreta: devuelve lo que haya.

**`settings.gui.set`** `→ {value: object | null}` `← {}`
- `value` tiene que ser un objeto, de **64 KiB** como máximo medido sobre el JSON compacto, o `null` para borrar la llave. Se guarda compacto, con sus llaves en el orden en que llegaron.
- Funciona con una respuesta en curso: no toca la sesión del chat.
- Errores: `-32602` sin `value`; `-32005` si no es objeto ni `null` o pasa de 64 KiB; `-32007` si falla la escritura.
- **Solo** la llave `"gui"`: no hay un método genérico para escribir secciones, para que nadie se salte las validaciones de `addons` o `appearance`.

`"appearance"` (tema, fondo y símbolos de la terminal) es solo de la TUI y no se expone.

### 6.8 Addons (solo Linux)

En Windows todos responden `-32006 Unsupported`.

**`addons.list`** `→ {}` `← {addons: [{name, installed, problem, config_version_ok}]}`
- Usa `known_addons`, `find_addon_executable` y `addon_install_problem`.

**`addons.check_install`** `→ {name}` `← igual que un elemento de la lista` · *larga, cancelable*
- Corre `chatbot-telegram --config-version` (`parse_config_version`).

**`addons.telegram.get`** `→ {}` `←`

```jsonc
{"form":{"enabled":true, "log_error_details":false,
         "fields":{"token":"", "model":"...", "model_key":"", "search_key":"",
                   "allowed_users":"123, 456", "invite_code":"...", "daily_limit":"50",
                   "concurrent":"2", "idle_minutes":"30", "stale_minutes":"10",
                   "draft_ms":"700", "time_zone":"America/Mexico_City"},
         "remove_model_key":false, "remove_search_key":false},
 "secrets":{"token":"...", "model_key":"...", "search_key":"..."},   // addon_secret_status (enmascarado)
 "labels":{"token":"Token", "model":"Modelo", ...},   // addon_field_label, para que la GUI no los copie
 "env_note":""}
```

Los campos de secretos van vacíos (vacío = se conserva lo guardado), igual que en `AddonForm`. Los nombres de `fields` son los de `AddonField` en snake_case.

**`addons.telegram.check`** `→ {form}` `← {field_errors: {campo: mensaje}, form_error, needs_restart}`
- Usa `addon_field_error` por campo y `addon_form_error`.

**`addons.telegram.save`** `→ {form}` `← {needs_restart}`
- Escribe `addons.telegram` y los cambios de `credentials.json` (`addon_credential_changes`).
- Después, la GUI pregunta "¿Reiniciar ahora?" y llama `service.restart`, o `service.reload` si no hace falta reiniciar. Es lo mismo que hace F2.

**`addons.telegram.invite_code`** `→ {}` `← {code}` (`generate_invite_code`; no guarda nada)

**`addons.telegram.invited.list`** `→ {}` `← {users: ["123456789", ...]}`

**`addons.telegram.invited.remove`** `→ {user}` `← {users: [...]}` (`remove_invited` bajo `flock`)

#### Servicio de systemd

Todas son *largas y cancelables* (corren `systemctl` con `child_process`).

**`service.status`** `→ {addon}` `←`

```jsonc
{"systemd":true, "active":"active", "enabled":"enabled", "exit_status":null,
 "unit":"current",              // missing | foreign | current | stale (UnitState)
 "linger":true, "last_error":"",
 "summary":"...",              // service_summary
 "problem":"", "mismatch":"",                          // service_problem, service_mismatch
 "journal_command":"..."}      // journal_command
```

**`service.apply`** `→ {addon, enabled, replace_foreign?: false}` `← {message}`
- Si hay una unidad ajena y `replace_foreign` es false: `-32008 ConfirmationRequired` con el texto de `kReplaceForeignUnit`. La GUI pregunta y repite con `true`.
- Sin systemd de usuario: `-32006` con el mensaje de `NoSystemd`.

**`service.restart`** · **`service.disable`** · **`service.reload`** `→ {addon}` `← {message}`

Todas emiten `service.changed {addon}` al terminar.

---

## 7. Eventos (notificaciones del servidor)

| Evento | Params | Cuándo |
|---|---|---|
| `conversation.entry_added` | `{conversation, rev, index, entry}` | Entrada nueva (usuario, respuesta, error, aviso, fuentes) |
| `conversation.entry_delta` | `{conversation, rev, index, text}` | Más texto en la respuesta en curso: **se concatena** |
| `conversation.entry_updated` | `{conversation, rev, index, entry}` | Cambiaron las marcas (termina, se cancela, nota) |
| `conversation.phase` | `{conversation, rev, phase}` | idle / searching / thinking / streaming |
| `conversation.saved` | `{conversation, rev, id, title}` | Se guardó después de un par (la primera vez trae id y título). Si el guardado falla no llega: llega la entrada de error |
| `conversation.closed` | `{conversation, reason}` | `replaced` (`new`, `open`), `deleted`, o `shutdown`: siempre en `shutdown` si hay sesión, con o sin respuesta en curso |
| `chat.finished` | ver abajo | Una vez por cada `chat.send` aceptado, salvo si antes llegó `shutdown` (sección 3) |
| `conversations.changed` | `{}` | Se guardó, borró o creó un archivo: la GUI vuelve a pedir la lista |
| `settings.changed` | `{section}` | Se guardó: `provider` / `system_prompt` / `search` / `gui` (`telegram` llega en R4). Sale después de la respuesta y antes de los avisos en el historial |
| `service.changed` | `{addon}` | Terminó una acción del servicio |

```jsonc
// chat.finished
{"conversation":"c1", "rev":57,
 "outcome":"ok",              // ok | error | cancelled | no_results
 "finish_reason":"stop",      // solo con ok; "" si no llegó o no fue ok
 "dropped_messages":3,        // recorte del historial: "se omitieron N mensajes antiguos"
 "error":null,                // ChatError con outcome = error
 "restore_text":null}         // con error / cancelled / no_results: el texto vuelve a la caja
```

- **`outcome`:** `ok` si terminó bien; `cancelled` con `kind: cancelled`; `no_results` si la búsqueda no trajo resultados; `error` con cualquier otro (y entonces `error` trae el `ChatError`). Una respuesta que termina sin texto es `error` con `kind: "bad_response"` y `El modelo no devolvió texto.`, como la entrada de error de la TUI.
- **Agrupar deltas:** el servidor junta los deltas que lleguen en una ventana de **30 ms** en un solo `entry_delta`. Unas 33 líneas por segundo como máximo, sin que se note. El cliente nunca debe suponer que un delta trae una palabra completa ni un token. Cualquier otro evento, y cualquier respuesta a una petición, primero vacía el delta pendiente: un `conversation.get` nunca trae texto que después llegue repetido.
- **Orden:** la respuesta a una petición sale antes que los eventos que provocó (`{accepted: true}` antes que el `entry_added` del usuario). Dentro de un cambio: el texto pendiente, la entrada que cambió de marcas, las entradas nuevas, `conversation.saved` y `conversations.changed`, `phase` y al último `chat.finished`.
- **Respuesta con búsqueda:** `entry_added` (usuario) → `phase: searching` → `phase: thinking` → `entry_added` (asistente, con el primer texto) → `phase: streaming` → `entry_delta`… → `entry_updated` (termina) → `entry_added` (fuentes) → `conversation.saved` → `conversations.changed` → `phase: idle` → `chat.finished`.
- `chat.finished` va **al último**: cuando llega, la conversación ya está guardada y en `idle`. Si el guardado falla no hay `conversation.saved`: llega una entrada `error` (`No se pudo guardar la conversación: …`) y `chat.finished` sigue siendo `ok`.

---

## 8. Ejemplo completo

```jsonc
→ {"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocol_version":1,"client":{"name":"chatbot-gui","version":"0.1.0"}}}
← {"jsonrpc":"2.0","id":1,"result":{"protocol_version":1,"ready":true,"platform":"linux","conversation":"c1", ...}}
→ {"jsonrpc":"2.0","id":2,"method":"conversation.get","params":{"conversation":"c1"}}
← {"jsonrpc":"2.0","id":2,"result":{"conversation":"c1","rev":0,"id":null,"title":"","phase":"idle","entries":[]}}
→ {"jsonrpc":"2.0","id":3,"method":"chat.send","params":{"conversation":"c1","text":"hola"}}
← {"jsonrpc":"2.0","id":3,"result":{"accepted":true}}
← {"jsonrpc":"2.0","method":"conversation.entry_added","params":{"conversation":"c1","rev":1,"index":0,"entry":{"kind":"user","text":"hola", ...}}}
← {"jsonrpc":"2.0","method":"conversation.phase","params":{"conversation":"c1","rev":2,"phase":"thinking"}}
← {"jsonrpc":"2.0","method":"conversation.entry_added","params":{"conversation":"c1","rev":3,"index":1,"entry":{"kind":"assistant","text":"¡Ho","in_progress":true, ...}}}
← {"jsonrpc":"2.0","method":"conversation.phase","params":{"conversation":"c1","rev":4,"phase":"streaming"}}
← {"jsonrpc":"2.0","method":"conversation.entry_delta","params":{"conversation":"c1","rev":5,"index":1,"text":"la! ¿En qué te ayudo?"}}
← {"jsonrpc":"2.0","method":"conversation.entry_updated","params":{"conversation":"c1","rev":6,"index":1,"entry":{"kind":"assistant","text":"¡Hola! ¿En qué te ayudo?","in_progress":false, ...}}}
← {"jsonrpc":"2.0","method":"conversation.saved","params":{"conversation":"c1","rev":7,"id":"20261025-185200-a1b2c3","title":"hola"}}
← {"jsonrpc":"2.0","method":"conversations.changed","params":{}}
← {"jsonrpc":"2.0","method":"conversation.phase","params":{"conversation":"c1","rev":8,"phase":"idle"}}
← {"jsonrpc":"2.0","method":"chat.finished","params":{"conversation":"c1","rev":9,"outcome":"ok","finish_reason":"stop","dropped_messages":0,"error":null,"restore_text":null}}
```

La primera entrada del asistente llega con `entry_added` (la crea el primer delta, como `append_delta`), y desde ahí con `entry_delta`.

---

## 9. Paridad TUI → RPC

| En la TUI | En el RPC |
|---|---|
| Arranque sin proveedor → F2 con aviso | `initialize.ready` / `problem` |
| Avisos de arranque en la línea de estado | `initialize.warnings` |
| Enter en la caja | `commands.run` (o `chat.send` directo) |
| Menú `/` con disponibilidad | `commands.list` |
| Esc durante la respuesta o la búsqueda | `chat.cancel` |
| "Pensando…" / "Buscando en la web…" | `conversation.phase` |
| Historial recortado (línea de estado) | `chat.finished.dropped_messages` |
| El texto vuelve a la caja tras un error | `chat.finished.restore_text` |
| Barra: grupos por fecha, ilegibles, abierta | `conversations.list` (`group`, `readable`, `open_as`) |
| Ctrl+N / `+ Nueva` | `conversation.new` |
| Abrir desde la barra (con aviso de modelo) | `conversation.open` |
| Supr + `s` | (confirmación en la GUI) + `conversation.delete` |
| `[Copiar]` / `/copiar` | `blocks.get` + portapapeles de Flutter / `commands.run` |
| `[Guardar]` / `/guardar` | `blocks.save` / `commands.run` |
| `[Exportar]` / `/exportar` | `conversation.export` / `commands.run` |
| Fuentes de `/buscar` | entrada `sources` |
| F2 → Proveedor de IA (lista, filtro, a mano, `CHAT_*`) | `settings.provider.*`, `models.list` |
| F2 → Instrucciones del sistema (contador, Restaurar) | `settings.system_prompt.*` |
| F2 → Búsqueda web | `settings.search.*` |
| F2 → Colores y Accesibilidad | **No aplica:** la GUI tiene su tema (`settings.gui.*`) |
| F2 → Addons (formulario, invitados, código) | `addons.*` |
| Servicio: aplicar, reiniciar, desactivar, reemplazar unidad ajena | `service.*` + `-32008` |
| Modo portable | Transparente: el servidor resuelve las rutas (`initialize.paths`) |

**Lo que se queda solo en la TUI:** pegado entre corchetes y su salida de emergencia, `TextArea`, ratón, QuickEdit, símbolos de respaldo, `CHAT_DEBUG_KEYS`, OSC 52 y los programas de portapapeles, y scroll y `HistoryView`. En Flutter todo eso lo resuelve el framework.

---

## 10. Seguridad y privacidad

- Sin red local: solo stdio (sección 2).
- Los secretos nunca salen del servidor (sección 4). Las pruebas lo vigilan: ninguna respuesta ni evento contiene la key que se mandó (ver 12).
- **Registro de depuración:** `CHAT_DEBUG_RPC=/ruta` escribe cada línea que entra y sale, con los valores de `api_key`, `token`, `model_key` y `search_key` reemplazados por `"•••"` **antes** de escribir. Igual que `CHAT_DEBUG_SSE`, contiene la conversación: solo para diagnosticar. Nunca está activo por defecto.
- El servidor no ejecuta nada que mande el cliente. `service.*` solo corre `systemctl` con argumentos fijos, como F2.

---

## 11. Versiones

- `protocol_version` es un entero y empieza en 1.
- **Sin cambio de versión:** métodos, eventos o campos opcionales nuevos. El cliente ignora lo que no conoce. Si un cliente necesita saber si algo existe, lo anuncia `capabilities`.
- **Con cambio de versión:** quitar o renombrar algo, cambiar un tipo o cambiar el significado de un campo.
- La GUI y `chatbot-rpc` se distribuyen juntos (mismo ZIP o carpeta), así que en la práctica la versión solo atrapa una instalación mezclada.

---

## 12. Pruebas

En el repositorio, `chatbot_rpc_tests` (sin red):

1. **Puras:** `LineReader` (tope de 1 MiB, línea partida, `\r\n`, EOF), `diff_entries`, la codificación de `Entry`, `ChatError` y los errores, `check_params` y la redacción de `CHAT_DEBUG_RPC`.
2. **`Dispatcher`** con la misma `Session` falsa que las pruebas de la TUI (`tests/session_fakes.hpp`) y un reloj falso: secuencias exactas de mensajes (el ejemplo de la sección 8, búsqueda, cancelar en cada fase, `Busy`, 429, `conversation.new`, guardado que falla, deltas agrupados, errores del protocolo), la configuración (`settings.*`, con y sin `CHAT_*` y con una respuesta en curso) y `models.list` con un transporte falso o bloqueante (lista, falla, cancelada, reemplazada, pendiente al `shutdown`); ninguna respuesta ni evento trae la key que se mandó.
3. **Proceso real** (solo POSIX): `chatbot-rpc` con pipes y un entorno limpio, sin configuración: `initialize`, `chat.send` → `-32004`, `shutdown`, `exit` → 0; EOF → 0 en menos de 3 s; `exit` sin `shutdown` → 1; toda línea de stdout es JSON-RPC 2.0; sin el aviso de stdin en terminal.
4. **Windows, en la CI:** `tools/check-rpc-pipes.sh` sobre `chatbot-rpc.exe` con pipes de MSYS2, en una carpeta portable: el ciclo de vida, ningún `\r` en la salida, entrada con `\r\n`, UTF-8 de ida y vuelta con `settings.gui.*`, EOF y stdout solo del protocolo; y en el ZIP, sin MSYS2 en el `PATH`, con y sin `chatbot.portable`. También corre a mano en Linux.
5. Las tres primeras corren también con `asan` y `tsan`.

Probar a mano, sin GUI:

```bash
cmake --preset dev && cmake --build --preset dev
printf '%s\n' \
  '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocol_version":1,"client":{"name":"manual","version":"0"}}}' \
  '{"jsonrpc":"2.0","id":2,"method":"conversations.list","params":{}}' \
  '{"jsonrpc":"2.0","id":3,"method":"shutdown","params":{}}' \
  '{"jsonrpc":"2.0","method":"exit"}' \
| ./build/dev/rpc/chatbot-rpc | jq -c .
```

Para una sesión interactiva: `rlwrap ./build/dev/rpc/chatbot-rpc 2>rpc.log`.

---

## 13. Lado de Dart (referencia)

Paquete [`json_rpc_2`](https://pub.dev/packages/json_rpc_2): maneja ids, errores y notificaciones. Solo hay que darle un canal de líneas.

```dart
import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'package:json_rpc_2/json_rpc_2.dart';
import 'package:stream_channel/stream_channel.dart';

Future<Peer> startServer(String exe) async {
  final proc = await Process.start(exe, const []);
  proc.stderr.transform(utf8.decoder).listen(stderr.write); // al log de la app

  final incoming = proc.stdout
      .transform(utf8.decoder)
      .transform(const LineSplitter());
  final outgoing = StreamController<String>();
  outgoing.stream.listen((line) => proc.stdin.write('$line\n'));

  final peer = Peer(StreamChannel<String>(incoming, outgoing.sink));

  peer.registerMethod('conversation.entry_delta', (Parameters p) {
    // conversations[p['conversation'].asString].append(p['index'].asInt, p['text'].asString);
  });
  peer.registerMethod('chat.finished', (Parameters p) { /* ... */ });
  peer.registerFallback((Parameters p) {}); // eventos que esta versión no conoce

  unawaited(peer.listen());
  await peer.sendRequest('initialize', {
    'protocol_version': 1,
    'client': {'name': 'chatbot-gui', 'version': '0.1.0'},
  });
  return peer;
}
```

- `proc.stdin.write('$line\n')`, no `writeln`, para que el salto siempre sea `\n`.
- Al cerrar la app: `shutdown`, `exit`, y si el proceso no sale en 3 s, `proc.kill()`.
- Un error JSON-RPC llega como `RpcException` con `code`, `message` y `data`.

---

## 14. Fuera de alcance de la versión 1

- Renombrar conversaciones (aún no existe en la TUI; está en "Ideas para después").
- Tool calling y "ChatBot Code".
- Varias conversaciones abiertas y respuestas en paralelo (el handle ya va en cada evento para poder agregarlas sin cambiar de versión).
- Avisar cambios hechos por **otro** proceso en la carpeta de conversaciones (otra TUI). La GUI puede volver a pedir `conversations.list` al recibir el foco.
- Adjuntar archivos o imágenes.
