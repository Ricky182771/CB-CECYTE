# Chatbot de CECYTE

[![CI](https://github.com/Ricky182771/CB-CECYTE/actions/workflows/ci.yml/badge.svg)](https://github.com/Ricky182771/CB-CECYTE/actions/workflows/ci.yml)

Repositorio para almacenar el código del chatbot hecho por estudiantes de CECyTE como sevicio social.

## Compilar y ejecutar

### Dependencias

Fedora:

```bash
sudo dnf install gcc-c++ cmake ninja-build libcurl-devel json-devel libasan libubsan libtsan
```

Ubuntu/Debian:

```bash
sudo apt install g++ cmake ninja-build libcurl4-openssl-dev nlohmann-json3-dev catch2
```

La interfaz usa [FTXUI](https://github.com/ArthurSonzogni/FTXUI) 7.x. Si no está instalada, CMake descarga la v7.0.3 al configurar. Para el markdown usa [md4c](https://github.com/mity/md4c) v0.6.0, que CMake siempre descarga (los paquetes de las distribuciones son versiones anteriores, sin notas al pie). Por eso la primera vez necesitas conexión a internet.

Las pruebas necesitan Catch2 v3:

- Ubuntu 24.04 (trae la 3.4.0) y Debian 13 "trixie" (trae la 3.7.1): el paquete `catch2` de `apt` basta.
- Debian 12 "bookworm": su paquete `catch2` es la v2 (2.13.10), así que CMake descarga la v3 por FetchContent al configurar. Otra opción es instalar la 3.7.1 desde `bookworm-backports`.

### Compilar para usar el chatbot

```bash
cmake --preset release && cmake --build --preset release
```

El programa queda en `./build/release/cli/chatbot`. Es la build optimizada: con conversaciones largas o al cambiar el tamaño de la terminal responde varias veces más rápido que la de desarrollo.

### Compilar para desarrollar

```bash
cmake --preset dev  && cmake --build --preset dev  && ctest --preset dev  --output-on-failure
```

Otros presets:

- `asan`: AddressSanitizer + UndefinedBehaviorSanitizer.
- `tsan`: ThreadSanitizer.
- `release`: la build optimizada; sus pruebas también deben pasar.

Se usan igual, cambiando `dev` por el nombre del preset. Cada preset compila en `build/<preset>/`.

Las pruebas de tiempo siempre miden y reportan con `WARN`. Sus límites solo hacen fallar la prueba con `CHATBOT_STRICT_PERF=1` (por ejemplo, `CHATBOT_STRICT_PERF=1 ctest --preset release --verbose`). La CI activa esa variable solo en release, con GCC y Clang. Esto incluye parseo, reparto de tablas, dibujo del historial y latencia de cancelación/destrucción. Las esperas máximas entre hilos y la comprobación de que una espera no termine antes de tiempo siguen siendo verificaciones funcionales obligatorias.

### Integración continua

En cada pull request y en cada push a `main`, GitHub Actions (`.github/workflows/ci.yml`) compila el proyecto en Ubuntu 24.04 y corre las dos suites de pruebas. Usa GCC con los presets `dev`, `asan`, `tsan` y `release`, y Clang con `dev` y `release`. En todos, los warnings cuentan como errores. Además, comprueba que el programa, sin `CHAT_API_KEY` y sin terminal, salga con código 1 y el mensaje de error de configuración. La CI nunca usa una API key ni se conecta a ninguna API real: las pruebas usan un transporte falso.

### Configurar

La forma más fácil es la pantalla de configuración: ábrela con **F2** o con la fila `⚙ Configuración` de la barra. Si al arrancar falta la key o el modelo, el chatbot la abre solo con el aviso `Configura un proveedor para empezar.` (mientras tanto, la caja de entrada no envía nada). Ahí eliges el proveedor, escribes la API key (se ve como `•••`) y eliges el modelo de la lista que devuelve el servidor (`GET {URL base}/models`). Con el filtro, ↑/↓ mueven el resaltado y Enter elige; si la lista falla (por ejemplo, Anthropic no documenta ese endpoint), el error aparece en la lista y puedes escribir el modelo a mano. Tab y Shift+Tab cambian de campo; Guardar aplica los cambios sin reiniciar y Esc o Cancelar cierran sin guardar (si hay cambios, pregunta `¿Descartar los cambios? (s/n)`). No se abre mientras hay una respuesta en curso.

| Proveedor | URL base | Lista de modelos |
|---|---|---|
| NVIDIA NIM | `https://integrate.api.nvidia.com/v1` | sí |
| OpenAI | `https://api.openai.com/v1` | sí |
| Google Gemini | `https://generativelanguage.googleapis.com/v1beta/openai` | sí |
| Anthropic (Claude) | `https://api.anthropic.com/v1` | no documentada: escribe el modelo |
| Groq | `https://api.groq.com/openai/v1` | no confirmada |
| OpenRouter | `https://openrouter.ai/api/v1` | sí |
| Mistral | `https://api.mistral.ai/v1` | sí |
| DeepSeek | `https://api.deepseek.com` | sí |
| Together AI | `https://api.together.ai/v1` | sí |
| xAI (Grok) | `https://api.x.ai/v1` | no confirmada |
| Cerebras | `https://api.cerebras.ai/v1` | no confirmada |
| Ollama (local) | `http://localhost:11434/v1` | sí (sin key) |
| Personalizado… | la que escribas | — |

"No confirmada" no quiere decir que falle: el chatbot la pide igual y, si falla, te deja escribir el modelo.

**Dónde se guarda la key:** en `~/.config/chatbot/credentials.json` (o `$XDG_CONFIG_HOME/chatbot/credentials.json`), una por proveedor (y una por URL en los personalizados). El archivo se escribe con permisos 0600 y su carpeta con 0700. Si alguien le abre los permisos (grupo u otros), el chatbot no lo lee y te pide correr `chmod 600` sobre él. Al guardar, una key que empieza con `$`, trae espacios o saltos de línea, o mide menos de 20 caracteres no se guarda: suele ser el nombre de una variable (`$NIMKEY`) o una key cortada al copiarla. La key nunca va en `config.json`, en los volcados de depuración ni en las conversaciones guardadas. No copies `credentials.json` al repositorio (está en `.gitignore`).

**Servidor local (Ollama, llama.cpp…):** elige `Ollama (local)` o `Personalizado…` con una URL como `http://localhost:8080/v1`. Solo se acepta `http://` para `localhost`, `127.0.0.1` o `[::1]` (cualquier puerto), y ahí la key es opcional; cualquier otro servidor necesita `https://`, para que la key y la conversación no viajen sin cifrar.

**Variables de entorno:** también puedes configurar todo sin la pantalla:

```bash
export CHAT_API_KEY="tu-key-de-nvidia"
export CHAT_MODEL="nvidia/nemotron-3-super-120b-a12b"   # solo es un ejemplo
```

No hay modelo por defecto: los modelos se retiran con el tiempo. El del ejemplo fue el más rápido y constante en las pruebas del equipo contra NIM, pero no es un valor por defecto.

Lo que guarda la pantalla va a `~/.config/chatbot/config.json` (o `$XDG_CONFIG_HOME/chatbot/config.json`), que también puedes editar a mano. Al guardar se conservan las demás llaves del archivo. Nunca lleva la key:

```json
{
    "provider": "nvidia",
    "base_url": "https://integrate.api.nvidia.com/v1",
    "model": "nvidia/nemotron-3-super-120b-a12b",
    "timeout_seconds": 120,
    "history_limit": 32000,
    "appearance": { "theme": "catppuccin-mocha", "background": "theme" },
    "system_prompt": "Eres un asistente útil. Responde en español, de forma clara y concisa."
}
```

Las variables de entorno (`CHAT_API_KEY`, `CHAT_BASE_URL`, `CHAT_MODEL`, `CHAT_TIMEOUT`, `CHAT_HISTORY_LIMIT`) tienen prioridad sobre lo guardado. En la pantalla, un campo definido por una de ellas muestra su valor con `(definido por CHAT_…)` y no se puede editar (la key, siempre enmascarada: solo sus últimos 4 caracteres).

`CHAT_HISTORY_LIMIT` (o `history_limit` en el archivo) limita cuánto historial se envía en cada mensaje. Se mide en bytes del texto, más o menos un carácter por byte (los acentos y la ñ cuentan doble). El valor por defecto es 32000; `0` significa sin límite. Si la conversación pasa del límite, se omiten los mensajes más antiguos y la línea de estado lo avisa. Si el servidor rechaza la petición por demasiado larga, el chatbot recorta el historial a la mitad (del límite o del tamaño real, lo que sea menor) y reintenta una vez.

#### Colores y accesibilidad

En la configuración, la categoría **Colores y Accesibilidad** elige el tema y el fondo; se aplican al guardar, sin reiniciar, y se guardan en `config.json` como `"appearance"`.

- **Tema:** `Catppuccin Mocha` (por defecto) usa sus propios colores RGB, iguales en cualquier terminal. `De la terminal` usa la paleta de 16 colores de tu terminal, como antes de los temas.
- **Fondo:** `Del tema` pinta el fondo del tema en toda la pantalla; `Transparente` deja el fondo y el color de texto de tu terminal y solo colorea los acentos (encabezados, código, enlaces…). Con `De la terminal` no aplica.

Si `config.json` nombra un tema que no existe, se usa el de por defecto y la línea de estado lo avisa.

En todas las listas, la fila del cursor se marca con el color de selección (o invertida con `De la terminal`) solo mientras la lista tiene el foco, y `●` marca lo elegido. El campo o botón con el foco lleva su etiqueta con el color de selección.

Si tu terminal no muestra colores de 24 bits, se aproximan a 256 o 16. Con `NO_COLOR` definida y no vacía se usa automáticamente `De la terminal`, con fondo transparente y cursor invertido. Tema y fondo aparecen deshabilitados con la nota `Desactivado por NO_COLOR`; la configuración guardada se conserva.

#### Instrucciones del sistema

En la configuración, la categoría **Instrucciones del sistema** edita el mensaje de sistema que se manda al principio de cada conversación (también de las que abres desde la barra). Enter agrega un salto de línea; Tab y Shift+Tab cambian de campo. **Restaurar predeterminado** vuelve a poner las instrucciones de fábrica. Al guardar se aplican sin reiniciar y sin perder la conversación abierta: valen desde el siguiente mensaje.

Se guardan en `config.json` como `"system_prompt"`:

```json
{
    "model": "nvidia/nemotron-3-super-120b-a12b",
    "system_prompt": "Eres un tutor de matemáticas.\nExplica paso a paso y en español."
}
```

- **Sin la llave** (o tras «Restaurar predeterminado» y Guardar, que la borra): se usan las predeterminadas, `Eres un asistente útil. Responde en español, de forma clara y concisa.`
- **Cadena vacía (`""`)**: no se manda ningún mensaje de sistema. Sirve con modelos locales que rechazan el rol `system`.
- Si la llave no es una cadena, se usan las predeterminadas y la línea de estado lo avisa.

Al guardar, las instrucciones pueden medir como máximo 8000 bytes (el contador `N / 8000 bytes` está bajo el campo), deben ser UTF-8 válido y no pueden tener caracteres de control salvo saltos de línea y tabuladores; si no cumplen, no se guardan y la pantalla dice por qué. El recorte del historial nunca quita las instrucciones: si miden lo mismo que `history_limit` o más, se guardan igual, pero la pantalla avisa que dejarían poco o nada de espacio para la conversación. No hay variable de entorno para las instrucciones. Las conversaciones guardadas no las incluyen.

#### Búsqueda web

Escribe `/buscar` seguido de tu pregunta (por ejemplo, `/buscar quién ganó el último partido del América`). El chatbot consulta [Tavily](https://tavily.com), le pasa al modelo los resultados como datos y el modelo responde citando cada dato con `[n]`. Debajo de la respuesta aparece el bloque **Fuentes** con `[n] título (URL)`; ese bloque sale de la búsqueda, no del texto del modelo (que podría inventar direcciones), y Shift+clic sobre un enlace lo abre. Funciona con cualquier modelo.

- Solo `/buscar` en minúsculas, seguido de un espacio y una consulta. `/buscar` solo muestra cómo se usa; cualquier otro texto que empiece con `/` se envía como un mensaje normal.
- Mientras busca, la línea de estado dice `Buscando en la web…`. Esc cancela igual durante la búsqueda que durante la respuesta.
- Si la búsqueda falla o no trae resultados, no se pregunta al modelo: aparece el error (o el aviso `La búsqueda no encontró resultados.`) y el texto regresa a la caja.
- Se buscan 5 resultados en español de México con búsqueda segura (`safe_search`) siempre activa; no se puede desactivar.
- Las conversaciones guardadas conservan los resultados: al abrirlas de nuevo, las fuentes siguen ahí y el modelo vuelve a recibir los mismos datos.

**Cómo sacar la key:** entra a [app.tavily.com](https://app.tavily.com), crea una cuenta (no pide tarjeta) y copia la API key de tu panel. Pégala en la configuración (F2), categoría **Búsqueda web**, y guarda. Se guarda en `credentials.json` con la llave `"search:tavily"`, junto a las demás keys y con los mismos permisos; nunca en `config.json`. También puedes usar la variable `CHAT_SEARCH_API_KEY`, que tiene prioridad y bloquea el campo. Sin key, `/buscar` no se envía y la línea de estado dice `Configura la key de búsqueda en Configuración (F2) → Búsqueda web`.

**Límites del plan gratuito:** 1,000 créditos al mes; cada `/buscar` gasta 1 (búsqueda básica). Al agotarlos aparece `Se agotaron las búsquedas del plan de Tavily`. No hay reintentos automáticos: vuelve a enviar con Enter cuando quieras.

**Privacidad:** a Tavily solo se envía la consulta que escribes después de `/buscar`, nunca la conversación. Lo que llega de las páginas se le pasa al modelo marcado como datos, con la instrucción de ignorar cualquier orden que venga dentro.

#### Conversaciones guardadas

Cada conversación se guarda sola después de cada respuesta completa, en un archivo `.json` por conversación dentro de:

- `CHAT_DATA_DIR`, si la defines;
- si no, `$XDG_DATA_HOME/chatbot/conversations` (solo si `XDG_DATA_HOME` es una ruta absoluta);
- si no, `~/.local/share/chatbot/conversations`.

Los archivos contienen **el texto completo de tus conversaciones**. Por eso la carpeta se crea con permisos 0700 y los archivos con 0600 (solo tu usuario puede leerlos). Para borrar una conversación, usa Supr en la barra de conversaciones (Ctrl+O); para borrarlas todas, borra la carpeta. Si un archivo está dañado o es de una versión más nueva del programa, aparece en la barra como ilegible y el programa nunca lo modifica.

#### Depuración

`CHAT_DEBUG_SSE=/ruta/archivo` (solo por variable de entorno) guarda en ese archivo, por cada intento de petición (tanto las respuestas por fragmentos, que usa la interfaz, como las completas, que usa `tools/smoke` sin `--stream`), la fecha, el modelo, el número de intento, el estado HTTP y el cuerpo crudo de la respuesta. Nunca guarda la key, las cabeceras ni lo que tú envías, pero **sí contiene las respuestas del modelo, es decir, la conversación**. Úsalo solo para diagnosticar un problema y borra el archivo después. Si no se puede escribir, el chatbot sigue funcionando sin avisar.

```bash
CHAT_DEBUG_SSE=/tmp/chat-debug.txt ./build/release/cli/chatbot
```

### Ejecutar

```bash
./build/release/cli/chatbot
```

Teclas:

| Tecla | Acción |
|---|---|
| Enter | Envía el mensaje (no hace nada mientras hay una respuesta en curso) o ejecuta un comando (`/buscar`, `/copiar`, `/guardar`, `/exportar`) |
| Esc | Cancela la respuesta en curso; el texto regresa a la caja |
| PgUp / PgDn | Sube o baja una pantalla del historial |
| Rueda del ratón | Sube o baja unas 3 líneas (del historial, o de la barra si el puntero está sobre ella) |
| Home / End | Con la caja vacía, va al inicio o al final del historial; con texto, mueve el cursor de la caja |
| F2 | Abre o cierra la configuración |
| Ctrl+N | Empieza una conversación nueva |
| Ctrl+B | Muestra u oculta la barra de conversaciones |
| Ctrl+O | Pasa a la barra de conversaciones (y la muestra si estaba oculta) |
| Ctrl+C | Sale |

#### Barra de conversaciones

A la izquierda está la lista de conversaciones guardadas, agrupadas por fecha: **Hoy**, **Ayer**, **Últimos 7 días** y después un grupo por día (`2 oct`, o `15 dic 2025` si es de otro año). La primera fila, `+ Nueva`, empieza una conversación nueva, y la segunda, `⚙ Configuración`, abre la configuración. `●` marca la conversación abierta, y las ilegibles aparecen tenues, con `(ilegible)`, en el grupo **Sin fecha** al final.

- Se ve al arrancar si la terminal mide 100 columnas o más. Si la terminal se angosta a menos de 100, se oculta sola, y reaparece al volver a ensancharla (salvo que la hayas ocultado tú con Ctrl+B).
- Para cambiar su ancho, arrastra con el ratón su borde derecho (de 18 a 60 columnas, o hasta la mitad de la terminal). El ancho no se guarda al salir.
- Con Ctrl+O la barra toma el foco: ↑/↓, PgUp/PgDn y Home/End para moverte (los encabezados de grupo se saltan); Enter abre; Supr borra (pide confirmación en la línea de estado: solo `s` borra); Esc vuelve a la caja de entrada. La fila seleccionada solo se resalta mientras la barra tiene el foco.
- Con el ratón: un clic en una conversación la abre, en `+ Nueva` empieza una nueva y en `⚙ Configuración` abre la configuración.
- Mientras hay una respuesta en curso, abrir, borrar o crear una conversación (desde la barra o con Ctrl+N) no hace nada: espera la respuesta o cancélala con Esc. Moverte por la lista sí se puede.

Si subes en el historial, la vista se queda donde está aunque llegue texto nuevo, y la línea de estado muestra `↓ Hay más abajo (End)`, o `(PgDn)` si hay texto en la caja (porque ahí End mueve el cursor). Al enviar un mensaje, la vista regresa abajo.

El chatbot captura el ratón para la rueda. Para seleccionar texto con el ratón, mantén Shift mientras arrastras, como en la mayoría de terminales con apps que usan el ratón. Para abrir un enlace, Shift+clic sobre él (en Kitty también Ctrl+Shift+clic): un clic simple lo recibe el chatbot y no abre nada.

### Markdown en las respuestas

Las respuestas del asistente se muestran con formato de markdown, también mientras van llegando. Tus mensajes, los errores y los avisos se muestran como texto plano.

Se muestra:

- Párrafos, encabezados (`#` a `######`), citas (también anidadas) y líneas horizontales (`---`).
- **Negritas**, *cursivas*, ~~tachado~~, `==resaltado==` y `código en línea`.
- Listas con viñetas y numeradas, anidadas, y listas de tareas (`- [ ]` y `- [x]`).
- Bloques de código con su número y el lenguaje como título (`#3 · cpp`, o `#3` si no dice el lenguaje). Las líneas largas se parten, sin colores de sintaxis. El número sirve para `/copiar` y `/guardar`.
- Tablas con alineación por columna. Si no caben, el texto se ajusta dentro de las celdas; si ni así caben, cada fila se muestra como una tarjeta `Encabezado: valor`.
- Enlaces: el texto subrayado y la dirección al lado. En las terminales que lo soportan, el enlace se abre con Shift+clic.
- Imágenes como `[imagen: descripción]` con su dirección, porque la terminal no muestra imágenes.
- Alertas de GitHub (`> [!NOTE]`, `[!TIP]`, `[!IMPORTANT]`, `[!WARNING]`, `[!CAUTION]`).
- Notas al pie (`[^1]`), que aparecen al final de la respuesta.
- Fórmulas de LaTeX (`$...$` y `$$...$$`) como texto, sin convertir.

El HTML dentro de la respuesta se muestra tal cual, como texto. Los caracteres de control que podrían alterar la terminal (por ejemplo, las secuencias que empiezan con ESC) se reemplazan por `�`, también en tus mensajes y en los errores.

Para ver cómo se muestra un archivo markdown sin usar la API:

```bash
./build/release/tools/md_preview --width 60 tests/data/markdown_muestra.md
./build/release/tools/md_preview --theme terminal tests/data/markdown_muestra.md
```

`--width` es el ancho en columnas (80 si no lo pones). `--theme` elige el tema (`catppuccin-mocha` si no lo pones, o `terminal`) y `--background theme|terminal` el fondo. Si la salida es una terminal, se ve con colores y estilos; si la rediriges a un archivo, sale como texto plano.

### Copiar, guardar y exportar

Los bloques de código de las respuestas se numeran desde 1 en toda la conversación, en el orden en que aparecen; el número va en el título del bloque (`#3 · cpp`).

**Con el ratón.** Cada bloque lleva a la derecha de su título los botones `[Copiar]` y `[Guardar]`. Si el bloque es más alto que la pantalla, al bajar con la rueda los botones se quedan pegados a la primera fila visible del bloque y desaparecen con él cuando ya no queda ninguna fila de código a la vista.

- `[Copiar]` copia el bloque y cambia a `[✓ Copiado]` hasta que mueves el puntero fuera del botón o pasan 2 segundos.
- `[Guardar]` lo guarda con el nombre por defecto (`bloque-3.cpp`). Para otro nombre, usa `/guardar 3 suma.cpp`.
- `[Exportar]`, a la derecha de la barra de título, exporta la conversación. Sin respuestas que exportar se ve atenuado y no hace nada.
- El resultado aparece en la línea de estado, corto y con lo importante primero: `#1 copiado con wl-copy`, `#1 copiado con OSC 52 · si no pega, usa [Guardar]`, `#1 guardado en ~/Descargas/chatbot/bloque-1.cpp` o `No se pudo copiar #1 · usa [Guardar]`. Si no cabe, termina en `…`; el indicador de la derecha (`Pensando…`) nunca se recorta. Los botones no agregan nada a la conversación ni mueven la vista: puedes copiar un bloque de la mitad de la conversación sin que salte al final.
- Funcionan también mientras llega una respuesta: un bloque que sigue llegando se copia como va, y el aviso empieza con `⚠ #1 incompleto (aún no termina)`. Lo mismo con un bloque de una respuesta cancelada (`se canceló`) o cortada por un error (`se cortó`).
- En una terminal angosta los botones se acortan a `[C]` `[G]`; si tampoco caben, no se muestran (los comandos siguen funcionando). Con el puntero encima van en el color de selección del tema (invertidos con `NO_COLOR`).

**Con comandos**, para cuando no hay ratón (por ejemplo, por SSH desde una terminal que no lo manda) o prefieres el teclado. No se envían al modelo ni entran al historial: el resultado aparece como un aviso en la conversación.

| Comando | Qué hace |
|---|---|
| `/copiar` | Copia el último bloque de código de la conversación |
| `/copiar 3` | Copia el bloque #3 |
| `/guardar` o `/guardar 3` | Guarda el último bloque, o el #3, como `bloque-3.cpp` (la extensión sale del lenguaje) |
| `/guardar 3 suma.cpp` | Lo guarda con ese nombre (si no le pones extensión, se agrega la del lenguaje) |
| `/exportar` | Exporta la conversación a Markdown, como `conversacion-<id>.md` |

- Solo en minúsculas y con los argumentos separados por espacios. Cualquier otro texto que empiece con `/` (por ejemplo `/copiarx`) se envía como un mensaje normal.
- Con una respuesta en curso, la línea de estado dice `Espera a que termine la respuesta`. Si no hay bloques, el número no existe o sobran argumentos, la línea de estado explica cómo se usa y el texto se queda en la caja.
- `/guardar` y `/exportar` nunca sobrescriben: si el archivo ya existe, usan `suma-2.cpp`, `suma-3.cpp`… (hasta `-99`). Los archivos se crean con permisos 0644 (o menos, según tu `umask`), nunca como ejecutables (tampoco los `.sh`).
- El nombre que das es solo el nombre del archivo: no puede llevar `/`, `\`, `..` ni caracteres de control, ni empezar con `.`, y mide como máximo 100 bytes.
- El archivo exportado tiene el título, la fecha y el modelo, y cada pregunta (`## Tú`, tal como la escribiste, también el `/buscar …`) con su respuesta (`## Asistente`) y, si hubo búsqueda, sus fuentes (`### Fuentes`). Nunca incluye las instrucciones del sistema, los resultados de búsqueda que recibió el modelo, keys, avisos ni errores.

**Cómo se copia**, en este orden:

1. Si estás conectado por SSH (`SSH_CONNECTION` o `SSH_TTY`), primero con la secuencia OSC 52 de la terminal, para que llegue al portapapeles de tu máquina local.
2. Con un programa del sistema que esté en tu `PATH`: `termux-clipboard-set` en Termux (paquete `termux-api` y la app Termux:API), `wl-copy` en Wayland, o `xclip` o `xsel` en X11. En Fedora: `sudo dnf install wl-clipboard` (Wayland) o `sudo dnf install xclip` (X11).
3. Si nada de eso funciona, con OSC 52, y el aviso dice `si tu terminal no lo soporta, usa /guardar`. Kitty, WezTerm, foot, Alacritty y Ghostty soportan OSC 52; dentro de tmux se envía envuelta para tmux, y puede hacer falta `set -g allow-passthrough on` o `set -g set-clipboard on` en tu `~/.tmux.conf`.

OSC 52 solo manda bloques de hasta 100 000 bytes; para uno más grande, usa `/guardar`.

**Dónde se guarda:** en la carpeta `chatbot/` (se crea con permisos 0755, o menos según tu `umask`) dentro de la primera de estas que aplique:

1. `CHAT_DOWNLOAD_DIR`, si la defines;
2. tu carpeta de descargas de `~/.config/user-dirs.dirs` (`XDG_DOWNLOAD_DIR`, la que configura tu escritorio), si existe;
3. en Termux, `~/storage/downloads` (después de correr `termux-setup-storage`);
4. `~/Descargas` o `~/Downloads`, la que exista;
5. tu carpeta personal (`~`).

Para cambiarla, define la variable antes de abrir el chatbot:

```bash
CHAT_DOWNLOAD_DIR=~/proyectos/escuela ./build/release/cli/chatbot
```

