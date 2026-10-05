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

**Dónde se guarda la key:** en `~/.config/chatbot/credentials.json` (o `$XDG_CONFIG_HOME/chatbot/credentials.json`), una por proveedor (y una por URL en los personalizados). El archivo se escribe con permisos 0600 y su carpeta con 0700. Si alguien le abre los permisos (grupo u otros), el chatbot no lo lee y te pide correr `chmod 600` sobre él. La key nunca va en `config.json`, en los volcados de depuración ni en las conversaciones guardadas. No copies `credentials.json` al repositorio (está en `.gitignore`).

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
    "appearance": { "theme": "catppuccin-mocha", "background": "theme" }
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

Si tu terminal no muestra colores de 24 bits, se aproximan a 256 o 16. Con la variable `NO_COLOR` definida no se usa ningún color (solo negritas, subrayado, tenue e invertido): con un tema RGB, la fila del cursor deja de distinguirse; usa `De la terminal` si necesitas `NO_COLOR`.

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
| Enter | Envía el mensaje (no hace nada mientras hay una respuesta en curso) |
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
- Bloques de código con el lenguaje como título. Las líneas largas se parten, sin colores de sintaxis.
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
