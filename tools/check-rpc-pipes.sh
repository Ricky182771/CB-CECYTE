#!/usr/bin/env bash
# chatbot-rpc con pipes de verdad (la CI de Windows lo corre sobre el .exe
# nativo; también corre en Linux): ciclo de vida, saltos de línea, UTF-8, EOF
# y stdout solo del protocolo.
#
# Uso: tools/check-rpc-pipes.sh <chatbot-rpc> <linux|windows>
#
# Junto al ejecutable debe haber un chatbot.portable: así config.json y las
# conversaciones van a su datos/ y no se toca la configuración de la máquina.
# En Windows no basta con cambiar APPDATA: SHGetKnownFolderPath gana sobre esa
# variable.
set -euo pipefail
export LC_ALL=C # grep compara bytes, no caracteres.

rpc=$1
platform=$2
if [ ! -f "$(dirname "$rpc")/chatbot.portable" ]; then
    echo "Falta chatbot.portable junto a $rpc"
    exit 2
fi
unset CHAT_API_KEY CHAT_BASE_URL CHAT_MODEL CHAT_SEARCH_API_KEY CHAT_DATA_DIR CHAT_DOWNLOAD_DIR

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

init='{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocol_version":1,"client":{"name":"ci","version":"0"}}}'
gui_value='{"año":"ñandú 👍"}'
gui_set='{"jsonrpc":"2.0","id":2,"method":"settings.gui.set","params":{"value":'"$gui_value"'}}'
gui_get='{"jsonrpc":"2.0","id":3,"method":"settings.gui.get","params":{}}'
shutdown='{"jsonrpc":"2.0","id":9,"method":"shutdown","params":{}}'
exit_msg='{"jsonrpc":"2.0","method":"exit"}'

fail() {
    echo "FALLÓ: $*"
    exit 1
}

# Revisa la salida de una corrida: ningún \r y toda línea es un mensaje JSON-RPC 2.0.
check_output() {
    local out="$work/$1.out"
    [ -s "$out" ] || fail "$1: stdout vacío"
    if grep -q $'\r' "$out"; then
        od -c "$out" | head -20
        fail "$1: la salida trae \\r"
    fi
    if grep -vqE '^\{.*"jsonrpc":"2\.0".*\}$' "$out"; then
        grep -vnE '^\{.*"jsonrpc":"2\.0".*\}$' "$out" | head -5
        fail "$1: stdout trae algo que no es un mensaje JSON-RPC 2.0"
    fi
}

# rpc_piped <nombre>: chatbot-rpc con un pipe en stdin y otro en stdout (no
# archivos: en Windows ReadFile y WriteFile se portan distinto). El código de
# salida queda en $work/<nombre>.status.
rpc_piped() {
    {
        local code=0
        timeout 10 "$rpc" 2> "$work/$1.err" || code=$?
        echo "$code" > "$work/$1.status"
    } | cat > "$work/$1.out"
}

# run <nombre> <código esperado>: stdin desde $work/<nombre>.in.
run() {
    cat "$work/$1.in" | rpc_piped "$1"
    local status
    status=$(cat "$work/$1.status")
    if [ "$status" -ne "$2" ]; then
        cat "$work/$1.err"
        fail "$1: se esperaba el código $2 y salió $status"
    fi
    check_output "$1"
    echo "ok: $1 (código $status, $(wc -l < "$work/$1.out") líneas, sin \\r)"
}

# 1. Ciclo básico: initialize, shutdown y exit → 0; exit sin shutdown → 1.
printf '%s\n' "$init" "$shutdown" "$exit_msg" > "$work/ciclo.in"
run ciclo 0
grep -q '"ready":false' "$work/ciclo.out" || fail "ciclo: initialize no dice ready:false"
grep -q "\"platform\":\"$platform\"" "$work/ciclo.out" || fail "ciclo: platform no es $platform"
grep -q '"portable":true' "$work/ciclo.out" || fail "ciclo: portable no es true"
grep -qE '"config":"[^"]*datos(\\\\|/)config\.json"' "$work/ciclo.out" ||
    fail "ciclo: paths.config no está en datos/"
grep -q '"jsonrpc":"2.0","id":9,"result":{}' "$work/ciclo.out" || fail "ciclo: shutdown sin respuesta"
grep -o '"config":"[^"]*"' "$work/ciclo.out"

printf '%s\n' "$init" "$exit_msg" > "$work/sin-shutdown.in"
run sin-shutdown 1

# 2. Entrada con \r\n: se acepta igual.
printf '%s\r\n' "$init" "$shutdown" "$exit_msg" > "$work/crlf.in"
run crlf 0
grep -q '"jsonrpc":"2.0","id":1,"result":{' "$work/crlf.out" || fail "crlf: initialize sin respuesta"
grep -q '"jsonrpc":"2.0","id":9,"result":{}' "$work/crlf.out" || fail "crlf: shutdown sin respuesta"

# 3. UTF-8: lo que regresa settings.gui.get es byte por byte lo que se mandó.
printf '%s\n' "$init" "$gui_set" "$gui_get" "$shutdown" "$exit_msg" > "$work/utf8.in"
run utf8 0
grep -qxF '{"jsonrpc":"2.0","id":2,"result":{}}' "$work/utf8.out" || fail "utf8: settings.gui.set falló"
expected='{"jsonrpc":"2.0","id":3,"result":{"value":'"$gui_value"'}}'
if ! grep -qxF "$expected" "$work/utf8.out"; then
    grep '"id":3' "$work/utf8.out" | od -c | head -10
    fail "utf8: settings.gui.get no regresó los mismos bytes"
fi

# 4. EOF a la mitad (sin exit): sale con 0 en menos de 3 s después de cerrar stdin.
begin=$(date +%s%N)
(printf '%s\n' "$init"; sleep 1) | rpc_piped eof
status=$(cat "$work/eof.status")
elapsed_ms=$((($(date +%s%N) - begin) / 1000000 - 1000))
[ "$status" -eq 0 ] || { cat "$work/eof.err"; fail "eof: salió con $status"; }
[ "$elapsed_ms" -lt 3000 ] || fail "eof: tardó $elapsed_ms ms después de cerrar stdin"
check_output eof
grep -q '"jsonrpc":"2.0","id":1,"result":{' "$work/eof.out" || fail "eof: initialize sin respuesta"
echo "ok: eof (código 0, $elapsed_ms ms después de cerrar stdin)"

echo "chatbot-rpc con pipes: todo bien"
