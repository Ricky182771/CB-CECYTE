# Chatbot de CECYTE
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

### Compilar y correr las pruebas

```bash
cmake --preset dev  && cmake --build --preset dev  && ctest --preset dev  --output-on-failure
```

Otros presets:

- `asan`: AddressSanitizer + UndefinedBehaviorSanitizer.
- `tsan`: ThreadSanitizer.

Se usan igual, cambiando `dev` por el nombre del preset. Cada preset compila en `build/<preset>/`.

### Configurar

La API key **solo** se lee de la variable de entorno; nunca la pongas en un archivo del repositorio.

```bash
export CHAT_API_KEY="tu-key-de-nvidia"
export CHAT_MODEL="nvidia/nemotron-3-super-120b-a12b"   # solo es un ejemplo
```

No hay modelo por defecto: los modelos de NVIDIA NIM se retiran con el tiempo. Elige uno vigente en el catálogo de NIM. El del ejemplo fue el más rápido y constante en las pruebas del equipo contra NIM, pero no es un valor por defecto.

También puedes guardar el modelo y otros ajustes en `~/.config/chatbot/config.json` (o en `$XDG_CONFIG_HOME/chatbot/config.json`). Este archivo nunca lleva la key:

```json
{
    "base_url": "https://integrate.api.nvidia.com/v1",
    "model": "nvidia/nemotron-3-super-120b-a12b",
    "timeout_seconds": 120,
    "history_limit": 32000
}
```

Las variables de entorno (`CHAT_BASE_URL`, `CHAT_MODEL`, `CHAT_TIMEOUT`, `CHAT_HISTORY_LIMIT`) tienen prioridad sobre el archivo.

`CHAT_HISTORY_LIMIT` (o `history_limit` en el archivo) limita cuánto historial se envía en cada mensaje. Se mide en bytes del texto, más o menos un carácter por byte (los acentos y la ñ cuentan doble). El valor por defecto es 32000; `0` significa sin límite. Si la conversación pasa del límite, se omiten los mensajes más antiguos y la línea de estado lo avisa. Si el servidor rechaza la petición por demasiado larga, el chatbot recorta el historial a la mitad (del límite o del tamaño real, lo que sea menor) y reintenta una vez.

#### Conversaciones guardadas

Cada conversación se guarda sola después de cada respuesta completa, en un archivo `.json` por conversación dentro de:

- `CHAT_DATA_DIR`, si la defines;
- si no, `$XDG_DATA_HOME/chatbot/conversations` (solo si `XDG_DATA_HOME` es una ruta absoluta);
- si no, `~/.local/share/chatbot/conversations`.

Los archivos contienen **el texto completo de tus conversaciones**. Por eso la carpeta se crea con permisos 0700 y los archivos con 0600 (solo tu usuario puede leerlos). Para borrar una conversación, usa Supr en la lista (Ctrl+O); para borrarlas todas, borra la carpeta. Si un archivo está dañado o es de una versión más nueva del programa, aparece en la lista como ilegible y el programa nunca lo modifica.

#### Depuración

`CHAT_DEBUG_SSE=/ruta/archivo` (solo por variable de entorno) guarda en ese archivo, por cada intento de petición (tanto las respuestas por fragmentos, que usa la interfaz, como las completas, que usa `tools/smoke` sin `--stream`), la fecha, el modelo, el número de intento, el estado HTTP y el cuerpo crudo de la respuesta. Nunca guarda la key, las cabeceras ni lo que tú envías, pero **sí contiene las respuestas del modelo, es decir, la conversación**. Úsalo solo para diagnosticar un problema y borra el archivo después. Si no se puede escribir, el chatbot sigue funcionando sin avisar.

```bash
CHAT_DEBUG_SSE=/tmp/chat-debug.txt ./build/dev/cli/chatbot
```

### Ejecutar

```bash
./build/dev/cli/chatbot
```

Teclas:

| Tecla | Acción |
|---|---|
| Enter | Envía el mensaje (no hace nada mientras hay una respuesta en curso) |
| Esc | Cancela la respuesta en curso; el texto regresa a la caja |
| PgUp / PgDn | Sube o baja una pantalla del historial |
| Rueda del ratón | Sube o baja unas 3 líneas |
| Home / End | Con la caja vacía, va al inicio o al final del historial; con texto, mueve el cursor de la caja |
| Ctrl+N | Empieza una conversación nueva |
| Ctrl+O | Abre la lista de conversaciones guardadas |
| Ctrl+C | Sale |

En la lista de conversaciones: ↑/↓, PgUp/PgDn y Home/End para moverte; Enter abre; Supr borra (pide confirmación: solo `s` borra); Esc vuelve a la conversación sin cambiar nada. Mientras hay una respuesta en curso, Ctrl+N y Ctrl+O no hacen nada: espera la respuesta o cancélala con Esc.

Si subes en el historial, la vista se queda donde está aunque llegue texto nuevo, y la línea de estado muestra `↓ Hay más abajo (End)`, o `(PgDn)` si hay texto en la caja (porque ahí End mueve el cursor). Al enviar un mensaje, la vista regresa abajo.

El chatbot captura el ratón para la rueda. Para seleccionar texto con el ratón, mantén Shift mientras arrastras, como en la mayoría de terminales con apps que usan el ratón.

### Markdown en las respuestas

Las respuestas del asistente se muestran con formato de markdown, también mientras van llegando. Tus mensajes, los errores y los avisos se muestran como texto plano.

Se muestra:

- Párrafos, encabezados (`#` a `######`), citas (también anidadas) y líneas horizontales (`---`).
- **Negritas**, *cursivas*, ~~tachado~~, `==resaltado==` y `código en línea`.
- Listas con viñetas y numeradas, anidadas, y listas de tareas (`- [ ]` y `- [x]`).
- Bloques de código con el lenguaje como título. Las líneas largas se parten, sin colores de sintaxis.
- Tablas con alineación por columna. Si no caben, el texto se ajusta dentro de las celdas; si ni así caben, cada fila se muestra como una tarjeta `Encabezado: valor`.
- Enlaces: el texto subrayado y la dirección al lado. En las terminales que lo soportan, el enlace se abre con clic.
- Imágenes como `[imagen: descripción]` con su dirección, porque la terminal no muestra imágenes.
- Alertas de GitHub (`> [!NOTE]`, `[!TIP]`, `[!IMPORTANT]`, `[!WARNING]`, `[!CAUTION]`).
- Notas al pie (`[^1]`), que aparecen al final de la respuesta.
- Fórmulas de LaTeX (`$...$` y `$$...$$`) como texto, sin convertir.

El HTML dentro de la respuesta se muestra tal cual, como texto. Los caracteres de control que podrían alterar la terminal (por ejemplo, las secuencias que empiezan con ESC) se reemplazan por `�`, también en tus mensajes y en los errores.

Para ver cómo se muestra un archivo markdown sin usar la API:

```bash
./build/dev/tools/md_preview --width 60 tests/data/markdown_muestra.md
```

`--width` es el ancho en columnas (80 si no lo pones). Si la salida es una terminal, se ve con colores y estilos; si la rediriges a un archivo, sale como texto plano.
