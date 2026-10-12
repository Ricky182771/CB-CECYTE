#!/usr/bin/env bash
# Arma chatbot-windows-x64.zip desde un build release. Corre en la terminal
# "MSYS2 UCRT64" (la usa también la CI):
#
#   tools/package-windows.sh [--allow-missing-licenses] [build] [zip]
#
#   build  carpeta del build (por defecto, build/release)
#   zip    ZIP de salida (por defecto, <build>/../chatbot-windows-x64.zip)
#
# El ZIP lleva, sin carpeta de por medio:
#   chatbot.exe y las DLL de /ucrt64/bin que reporta ldd;
#   LICENSE.txt (el LICENSE del proyecto);
#   AVISOS-TERCEROS.txt: las licencias de lo que va adentro, sacadas de los
#     paquetes de MSYS2 (pacman -Qo dice de qué paquete es cada DLL) y de las
#     fuentes que bajó FetchContent (FTXUI y md4c, enlazadas estáticas);
#   LEEME.txt y chatbot.portable.ejemplo (tools/windows-dist/).
# Si un paquete de MSYS2 no instala su licencia, los textos salen de
# tools/windows-dist/licenses/<paquete sin prefijo>/ (del tarball oficial);
# si la versión instalada no es la de su README.md, avisa sin fallar.
# Los .txt van con saltos de línea CRLF, para el Bloc de notas.
#
# Si no encuentra el texto de alguna licencia, lo dice y falla sin armar el
# ZIP; con --allow-missing-licenses lo arma igual y AVISOS-TERCEROS.txt dice
# qué falta. Al final lista el ZIP y revisa que traiga exactamente lo esperado.
set -euo pipefail
export LC_ALL=C # La salida de pacman en inglés, para leer sus campos.

allow_missing=0
if [[ ${1:-} == --allow-missing-licenses ]]; then
    allow_missing=1
    shift
fi

die() {
    echo "package-windows.sh: $*" >&2
    exit 1
}

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
build=$(cd "${1:-$root/build/release}" && pwd) || die "no existe la carpeta del build"
zip=${2:-$(dirname "$build")/chatbot-windows-x64.zip}

[[ ${MSYSTEM:-} == UCRT64 ]] || die "corre esto en la terminal MSYS2 UCRT64 (MSYSTEM=${MSYSTEM:-})"
prefix=${MINGW_PREFIX:-/ucrt64}
for tool in ldd pacman cmake gcc; do
    command -v "$tool" > /dev/null || die "falta $tool"
done

exe=$build/cli/chatbot.exe
[[ -f $exe ]] || die "no existe $exe: compila antes con cmake --build --preset release"

# Ruta absoluta (se usa desde la carpeta de preparación) y sin el ZIP de
# una corrida anterior, para que un error no deje uno viejo como si fuera nuevo.
mkdir -p "$(dirname "$zip")"
zip=$(cd "$(dirname "$zip")" && pwd)/$(basename "$zip")
rm -f "$zip"

stage=$build/package
rm -rf "$stage"
mkdir -p "$stage"

# --- chatbot.exe y sus DLL ---------------------------------------------------

cp "$exe" "$stage/"
deps=$(ldd "$exe")
if grep -qi "not found" <<< "$deps"; then
    die "ldd no encontró alguna DLL:"$'\n'"$deps"
fi
# Las de Windows (/c/Windows/...) vienen con el sistema; cualquier otra que no
# sea de $prefix/bin (por ejemplo, de /usr/bin, de MSYS) no debería estar.
others=$(awk -v p="$prefix/bin/" '$3 != "" && index($3, p) != 1 &&
                                  tolower($3) !~ /^\/[a-z]\/windows\// { print $3 }' <<< "$deps")
[[ -z $others ]] || die "chatbot.exe usa DLL fuera de $prefix/bin y de Windows:"$'\n'"$others"
mapfile -t dlls < <(awk -v p="$prefix/bin/" 'index($3, p) == 1 { print $3 }' <<< "$deps" | sort -u)
[[ ${#dlls[@]} -gt 0 ]] || die "ldd no reportó ninguna DLL de $prefix/bin"
cp "${dlls[@]}" "$stage/"

# --- Licencias -----------------------------------------------------------------

missing=()
notices=$stage/AVISOS-TERCEROS.txt
rule='================================================================================'

# Textos guardados en el repositorio para los paquetes de MSYS2 que no
# instalan su licencia: tools/windows-dist/licenses/<paquete sin prefijo>/,
# con un README.md que dice de qué versión son ("- Versión: X").
repo_licenses=$root/tools/windows-dist/licenses
warnings=()

# Deja en $license_files los archivos de licencia de un paquete instalado
# (uno por línea; no se usa $(...) para que $warnings llegue): los que tiene bajo
# share/licenses/; si no tiene, los COPYING* / LICENSE* / LICENCE* que haya
# instalado en otro lado (por ejemplo, share/doc/); si tampoco, los de
# $repo_licenses (todos menos el README.md). Si la versión instalada no es la
# del README.md, avisa en $warnings (sin fallar), para revisar los textos.
package_license_files() {
    local package=$1 files name dir saved installed
    license_files=
    files=$(pacman -Qlq "$package" | grep -E "^$prefix/share/licenses/.+[^/]$" || true)
    if [[ -z $files ]]; then
        files=$(pacman -Qlq "$package" | grep -Ei '/(copying|licen[cs]e)[^/]*$' |
            grep -v '/$' || true)
    fi
    name=${package#"${MINGW_PACKAGE_PREFIX:-mingw-w64-ucrt-x86_64}"-}
    dir=$repo_licenses/$name
    if [[ -z $files && -d $dir ]]; then
        files=$(find "$dir" -maxdepth 1 -type f ! -name README.md | sort)
        saved=$(sed -n 's/^- Versión: *//p' "$dir/README.md" 2> /dev/null | head -n 1)
        installed=$(package_version "$package")
        installed=${installed#*:} # Sin la época (1:2.3.8-4).
        installed=${installed%-*} # Sin el número de compilación de MSYS2.
        if [[ $saved != "$installed" ]]; then
            local text="$package $installed: los textos de tools/windows-dist/licenses/$name"
            text+=" son de la versión ${saved:-(sin \"- Versión\" en su README.md)};"
            warnings+=("$text revísalos con el tarball de la $installed")
        fi
    fi
    license_files=$files
}

# Escribe una sección: título, datos y el texto de cada archivo de licencia.
#   section <título> <detalle> <archivos (uno por línea)>
section() {
    local title=$1 detail=$2 files=$3 file
    {
        printf '\n%s\n%s\n%s\n' "$rule" "$title" "$rule"
        printf '%s\n' "$detail"
        if [[ -z $files ]]; then
            printf '\nNO SE ENCONTRÓ EL TEXTO DE LA LICENCIA.\n'
        fi
    } >> "$notices"
    if [[ -z $files ]]; then
        missing+=("$title")
        return
    fi
    while IFS= read -r file; do
        {
            local shown=${file#"$prefix"/}
            shown=${shown#"$build"/}
            if [[ $file == "$repo_licenses"/* ]]; then
                shown="${file#"$root"/} (de las fuentes oficiales; el paquete no lo instala)"
            fi
            printf '\n--- %s ---\n\n' "$shown"
            cat "$file"
        } >> "$notices"
    done <<< "$files"
}

package_version() { pacman -Q "$1" | awk '{ print $2 }'; }
package_licenses() { pacman -Qi "$1" | sed -n 's/^Licenses *: //p'; }

# Sección de un paquete de MSYS2.
#   package_section <paquete> <cómo va incluido>
package_section() {
    local package=$1 how=$2
    package_license_files "$package"
    section "$package $(package_version "$package")" \
        "Incluido como: $how"$'\n'"Licencia según pacman: $(package_licenses "$package")" \
        "$license_files"
}

cat > "$notices" << EOF
AVISOS DE TERCEROS - chatbot para Windows (x64)

chatbot.exe es software libre bajo la Licencia Pública General de GNU,
versión 3 (ver LICENSE.txt). Lleva enlazados de forma estática FTXUI, md4c,
nlohmann/json y partes del runtime de GCC y de mingw-w64, y se distribuye
con las DLL de MSYS2 (UCRT64) que necesita. Abajo está la licencia de cada
uno, tal como la instala su paquete o como viene en sus fuentes.

Generado por tools/package-windows.sh.
EOF

# Enlazadas estáticas desde las fuentes de FetchContent (o desde el paquete
# de MSYS2, si CMake encontró FTXUI instalada).
ftxui_src=$build/_deps/ftxui-src
if [[ -f $ftxui_src/LICENSE ]]; then
    ftxui_version=$(sed -n 's/^ *VERSION \([0-9.]*\).*/\1/p' "$ftxui_src/CMakeLists.txt" | head -n 1)
    section "FTXUI ${ftxui_version:-(versión desconocida)}" \
        "Incluido como: enlazado estático en chatbot.exe (fuentes de FetchContent)" \
        "$ftxui_src/LICENSE"
elif pacman -Qq "${MINGW_PACKAGE_PREFIX:-mingw-w64-ucrt-x86_64}-ftxui" > /dev/null 2>&1; then
    package_section "${MINGW_PACKAGE_PREFIX:-mingw-w64-ucrt-x86_64}-ftxui" \
        "enlazado estático en chatbot.exe"
else
    section "FTXUI" "Incluido como: enlazado estático en chatbot.exe" ""
fi

md4c_src=$build/_deps/md4c-src
section "md4c 0.6.0" \
    "Incluido como: enlazado estático en chatbot.exe (solo la biblioteca de parseo;"$'\n'"fuentes de FetchContent)" \
    "$(ls "$md4c_src"/LICENSE* 2> /dev/null || true)"

# nlohmann/json es solo headers: el paquete dueño del header.
json_package=$(pacman -Qqo "$prefix/include/nlohmann/json.hpp" 2> /dev/null) ||
    die "no se encontró el paquete de nlohmann/json ($prefix/include/nlohmann/json.hpp)"
package_section "$json_package" "compilado dentro de chatbot.exe (biblioteca de solo headers)"

# Runtime de GCC enlazado estático (libgcc.a, crtbegin/crtend y el código de
# las plantillas de libstdc++): GPL 3 con la GCC Runtime Library Exception.
libgcc=$(gcc -print-libgcc-file-name)
gcc_package=$(pacman -Qqo "$libgcc") || die "no se encontró el paquete de $libgcc"
# El paquete del compilador puede no traer los textos; los de las DLL de GCC
# (gcc-libs, dueño de libstdc++-6.dll) son del mismo proyecto y con la misma
# licencia. Se usa el primero que traiga la excepción (COPYING.RUNTIME); si
# ninguno, el primero con algún texto, y se avisa que falta la excepción.
stdcxx_package=$(pacman -Qqo "$prefix/bin/libstdc++-6.dll" 2> /dev/null || true)
gcc_files=
gcc_from=$gcc_package
for candidate in "$gcc_package" $stdcxx_package; do
    package_license_files "$candidate"
    files=$license_files
    if grep -q 'COPYING\.RUNTIME' <<< "$files"; then
        gcc_files=$files
        gcc_from=$candidate
        break
    fi
    if [[ -z $gcc_files && -n $files ]]; then
        gcc_files=$files
        gcc_from=$candidate
    fi
done
section "Runtime de GCC ($gcc_package $(package_version "$gcc_package"))" \
    "Incluido como: partes enlazadas estáticas en chatbot.exe"$'\n'"Licencia según pacman: $(package_licenses "$gcc_package")"$'\n'"Textos tomados del paquete: $gcc_from" \
    "$gcc_files"
if [[ -n $gcc_files ]] && ! grep -q 'COPYING\.RUNTIME' <<< "$gcc_files"; then
    missing+=("GCC Runtime Library Exception (COPYING.RUNTIME)")
fi

# mingw-w64: el código de arranque y la parte estática del CRT (crt2.o,
# libmingwex).
crt_package=$(pacman -Qqo "$prefix/lib/crt2.o") || die "no se encontró el paquete de crt2.o"
package_section "$crt_package" \
    "partes enlazadas estáticas en chatbot.exe (arranque y CRT de mingw-w64)"

# Las DLL, agrupadas por paquete.
declare -A package_dlls=()
for dll in "${dlls[@]}"; do
    package=$(pacman -Qqo "$dll") || die "pacman no sabe de qué paquete es $dll"
    package_dlls[$package]+="${dll##*/} "
done
mapfile -t packages < <(printf '%s\n' "${!package_dlls[@]}" | sort)
for package in "${packages[@]}"; do
    package_section "$package" "${package_dlls[$package]% }"
done

for warning in "${warnings[@]}"; do
    echo "AVISO: $warning" >&2
    if [[ -n ${GITHUB_ACTIONS:-} ]]; then
        echo "::warning title=Licencia por revisar::$warning"
    fi
done

if [[ ${#missing[@]} -gt 0 ]]; then
    {
        printf '\n%s\nLICENCIAS QUE NO SE ENCONTRARON\n%s\n' "$rule" "$rule"
        printf -- '- %s\n' "${missing[@]}"
    } >> "$notices"
    echo "No se encontró el texto de la licencia de:" >&2
    printf -- '- %s\n' "${missing[@]}" >&2
    if [[ $allow_missing -eq 0 ]]; then
        die "falta el texto de alguna licencia (con --allow-missing-licenses se arma el ZIP igual)"
    fi
fi

# --- Textos y ZIP ---------------------------------------------------------------

cp "$root/LICENSE" "$stage/LICENSE.txt"
cp "$root/tools/windows-dist/LEEME.txt" "$root/tools/windows-dist/chatbot.portable.ejemplo" \
    "$stage/"
for text in LICENSE.txt LEEME.txt AVISOS-TERCEROS.txt chatbot.portable.ejemplo; do
    sed -i 's/\r*$/\r/' "$stage/$text"
done

expected=(chatbot.exe "${dlls[@]##*/}" LICENSE.txt AVISOS-TERCEROS.txt LEEME.txt
          chatbot.portable.ejemplo)
(cd "$stage" && cmake -E tar cf "$zip" --format=zip -- "${expected[@]}")

# Que el ZIP traiga exactamente lo esperado, ni más ni menos.
listed=$(cmake -E tar tf "$zip" | sed 's/\r$//' | sort)
wanted=$(printf '%s\n' "${expected[@]}" | sort)
if [[ $listed != "$wanted" ]]; then
    diff <(echo "$wanted") <(echo "$listed") >&2 || true
    die "el ZIP no trae exactamente lo esperado"
fi

echo "Listo: $zip"
echo "$listed"
