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

La interfaz usa [FTXUI](https://github.com/ArthurSonzogni/FTXUI) 7.x. Si no está instalada, CMake descarga la v7.0.3 al configurar, así que la primera vez necesitas conexión a internet. Lo mismo pasa con Catch2 v3 si tu distribución no la trae.

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
export CHAT_MODEL="meta/llama-3.1-8b-instruct"   # solo es un ejemplo
```

No hay modelo por defecto: los modelos de NVIDIA NIM se retiran con el tiempo. Elige uno vigente en el catálogo de NIM.

También puedes guardar el modelo y otros ajustes en `~/.config/chatbot/config.json` (o en `$XDG_CONFIG_HOME/chatbot/config.json`). Este archivo nunca lleva la key:

```json
{
    "base_url": "https://integrate.api.nvidia.com/v1",
    "model": "meta/llama-3.1-8b-instruct",
    "timeout_seconds": 120
}
```

Las variables de entorno (`CHAT_BASE_URL`, `CHAT_MODEL`, `CHAT_TIMEOUT`) tienen prioridad sobre el archivo.

### Ejecutar

```bash
./build/dev/cli/chatbot
```

Escribe tu mensaje y presiona Enter. Para salir, presiona Ctrl+C.
