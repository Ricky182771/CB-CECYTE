# Guía: 8. HTTP y API
> Las reglas resumidas están en [AGENTS.md](../AGENTS.md).
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
  - **3xx:** `CurlTransport` nunca sigue redirecciones (sin `CURLOPT_FOLLOWLOCATION`: libcurl convertiría el POST en GET y la conversación iría a otra URL sin avisar); guarda `CURLINFO_REDIRECT_URL` en `HttpResponse::redirect_url`. Siguen siendo `BadResponse`, sin reintento, pero con `redirect_url` el mensaje dice a dónde redirige (`redirect_display`: sin query, fragmento, usuario ni controles, ≤ 200 bytes) y, si la redirección termina en el mismo path, la base sugerida (`suggest_base_url`, solo si pasa `validate_base_url` y no es la actual). Si la key aparece en la URL limpia o en la sugerencia, el mensaje genérico. En "Proveedor de IA", "Personalizado…" con una URL sin ruta (`base_url_has_path`) muestra un aviso que no bloquea: casi todos usan `/v1`.
  - `Retry-After` (segundos enteros, con tope de 60 s) se respeta en todos los errores HTTP reintentables: 429 y 5xx.
  - **Errores dentro del flujo** (evento SSE `{"error": {...}}` con estado 200, como manda NIM la sobrecarga): si `error.code` es un número o una cadena numérica, se clasifica con el mismo mapeo de estados HTTP; si no, por `error.type` sin distinguir mayúsculas (`overload`, `unavailable` o `server` → `Server`; `rate` o `exhausted` → `RateLimited`); si no, por `error.message` sin distinguir mayúsculas (`overload` o `temporarily unavailable` → `Server`; `rate limit` o `too many requests` → `RateLimited`); si nada aplica, `BadResponse`. El mensaje es `error.message`, truncado. `complete_stream` los reintenta con la regla de siempre: solo si son reintentables y no se ha entregado ningún delta.
  - Fallo de curl: `Network` o `Timeout` según el código.

### Tavily (búsqueda web)

- `POST https://api.tavily.com/search`, `Authorization: Bearer <key>` (por `HttpRequest::api_key` y `CurlTransport`), timeout de 30 s. Cuerpo fijo: `query`, `search_depth: "basic"` (1 crédito), `max_results: 5`, `topic: "general"`, `safe_search: true` (no configurable: los usuarios son menores de edad), `country: "mexico"`, `include_published_date: true` (sin él, con `topic: "general"` no llega la fecha). Sin `include_answer`, `include_raw_content` ni imágenes.
- Respuesta: `results` debe ser arreglo (si no, `BadResponse`); se descarta un resultado sin `url` cadena que pase `is_web_url` o sin `content` cadena; `title` y `published_date` son opcionales; lo demás se ignora. `published_date` llega como RFC 1123 (`"Tue, 11 Mar 2025 17:00:00 GMT"`) o `null`; `normalize_published_date` (`core/src/published_date.h`, pura) la deja como `AAAA-MM-DD` si es ISO 8601 o RFC 1123 con fecha válida, y si no, tal cual. "Fuentes" la muestra: `[n] título — AAAA-MM-DD (url)`.
- Errores (el cuerpo es `{"detail": {"error": "..."}}`): 400 y 422 → `InvalidRequest`; 401 → `Auth` ("La key de búsqueda no es válida"); 429 → `RateLimited`; 432 y 433 → `RateLimited` ("Se agotaron las búsquedas del plan de Tavily"); 5xx → `Server`; otros → `BadResponse`. Sin respuesta HTTP (estado 0): `cancelled` o token cancelado → `Cancelled`; `timed_out` → `Timeout`; si no, `Network`, con un mensaje propio si el transporte no dio uno (nunca "Error HTTP 0"). Sin reintentos ni volcado de depuración.
  - Cuerpo que no se puede interpretar: `BadResponse`.
- En errores, intentar extraer el mensaje del cuerpo (`error.message` o `detail`); si no hay, usar el texto del estado HTTP. Truncar cuerpos largos.
- `base_url` se valida interpretando la URL: `https://` siempre; `http://` **solo** si el host es exactamente `localhost`, `127.0.0.1` o `[::1]` (cualquier puerto), para servidores locales como Ollama o llama.cpp; nunca con `usuario@` en la URL. Con host local la key es opcional y, si está vacía, `CurlTransport` no manda `Authorization`.
- Verificación TLS **siempre activa**. Timeout de conexión 10 s.
- `curl_global_init` se llama una sola vez (envuélvelo en RAII).

