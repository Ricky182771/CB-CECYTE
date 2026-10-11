# Revisa qué símbolos de la interfaz tienen las fuentes de la consola de
# Windows 10 (conhost): Consolas (consola.ttf) y Lucida Console (lucon.ttf).
# Lee el cmap de cada fuente con PresentationCore, que ya trae Windows
# PowerShell 5.1: no hay que instalar nada.
#
# Uso, desde la raíz del repositorio:
#
#   powershell -ExecutionPolicy Bypass -File tools\check_glyphs.ps1
#
# Opcional: -FontsDir <carpeta> para revisar las fuentes de otra carpeta.
#
# Imprime una fila por código con si/NO por fuente y al final los que faltan
# en alguna de las dos. Sale con 1 si la lista no cuadra con las fuentes: un
# símbolo de "quedan" que falta en alguna, o uno de "faltan" que está en las
# dos (sobra en la tabla).
#
# Los códigos van en hexadecimal (no los símbolos), para no depender de la
# codificación de este archivo. Son los de cli/src/glyphs.cpp: "faltan" =
# glyph_table() (los que se reemplazan en modo Sencillos) y "quedan" =
# kept_glyphs(). tests/test_glyphs.cpp falla si las listas se desincronizan:
# no cambies las marcas "glyphs: inicio" y "glyphs: fin".

param(
    [string]$FontsDir = (Join-Path $env:windir 'Fonts')
)

$ErrorActionPreference = 'Stop'

# glyphs: inicio
$faltan = @(
    0x2699, # Gear
    0x2713, # Check
    0x26A0, # Warning
    0x2611, # TaskDone
    0x2610, # TaskOpen
    0x25B4, # DropdownOpen
    0x25BE, # DropdownClosed
    0x258D, # HeadingMark
    0x25CF, # ChosenMark
    0x25E6, # BulletLevel2
    0x25AA, # BulletLevel3
    0x256D, # RoundedTopLeft
    0x256E, # RoundedTopRight
    0x2570, # RoundedBottomLeft
    0x256F, # RoundedBottomRight
    0x2503, # ScrollThumb
    0x2579, # ScrollThumbTop
    0x257B  # ScrollThumbBottom
)
$quedan = @(
    # Texto y avisos.
    0x00B7, 0x00AB, 0x00BB, 0x2014, 0x2026, 0x2192, 0x2191, 0x2193,
    # Viñeta del primer nivel, respaldo del elegido y la key enmascarada.
    0x2022,
    # Bordes y separadores rectos.
    0x2500, 0x2502, 0x250C, 0x2510, 0x2514, 0x2518, 0x251C, 0x2524, 0x252C,
    0x2534, 0x253C,
    # Respaldos que no son ASCII.
    0x221A, 0x258C, 0x2588, 0x2580, 0x2584
)
# glyphs: fin

Add-Type -AssemblyName PresentationCore

$fonts = @(
    @{ Name = 'Consolas'; File = 'consola.ttf' },
    @{ Name = 'Lucida Console'; File = 'lucon.ttf' }
)
foreach ($font in $fonts) {
    $path = Join-Path $FontsDir $font.File
    if (-not (Test-Path -LiteralPath $path)) {
        Write-Error "No existe $path"
    }
    $typeface = New-Object System.Windows.Media.GlyphTypeface -ArgumentList ([uri]$path)
    $font.Map = $typeface.CharacterToGlyphMap
}

$rows = @()
foreach ($group in @(@{ Name = 'falta'; Codes = $faltan }, @{ Name = 'queda'; Codes = $quedan })) {
    foreach ($code in $group.Codes) {
        $row = [ordered]@{ Codigo = ('U+{0:X4}' -f $code); Lista = $group.Name }
        $missing = $false
        foreach ($font in $fonts) {
            $has = $font.Map.ContainsKey([int]$code)
            $row[$font.Name] = if ($has) { 'si' } else { 'NO' }
            if (-not $has) { $missing = $true }
        }
        $row.FaltaEnAlguna = $missing
        $rows += [pscustomobject]$row
    }
}

$table = $rows | Format-Table Codigo, Lista, 'Consolas', 'Lucida Console' -AutoSize |
    Out-String -Width 200
Write-Output $table.Trim()
Write-Output ''

$missingRows = @($rows | Where-Object { $_.FaltaEnAlguna })
Write-Output 'Faltan en alguna de las dos:'
if ($missingRows.Count -eq 0) {
    Write-Output '  (ninguno)'
} else {
    Write-Output ('  ' + (($missingRows | ForEach-Object { $_.Codigo }) -join ' '))
}

$problems = @()
foreach ($row in $rows) {
    if ($row.Lista -eq 'queda' -and $row.FaltaEnAlguna) {
        $problems += "$($row.Codigo) se queda en modo Sencillos, pero falta en alguna fuente: hay que moverlo a glyph_table()."
    }
    if ($row.Lista -eq 'falta' -and -not $row.FaltaEnAlguna) {
        $problems += "$($row.Codigo) lo tienen las dos fuentes: no hace falta reemplazarlo."
    }
}
if ($problems.Count -gt 0) {
    Write-Output ''
    Write-Output 'La lista no cuadra con las fuentes:'
    $problems | ForEach-Object { Write-Output "  $_" }
    exit 1
}
exit 0
