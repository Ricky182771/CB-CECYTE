# Licencias de libidn2

El paquete `mingw-w64-ucrt-x86_64-libidn2` de MSYS2 no instala ningún archivo
de licencia (su `package()` solo hace `make install`), así que
`tools/package-windows.sh` toma de aquí los textos para `AVISOS-TERCEROS.txt`.

- Versión: 2.3.8
- Fuente: https://ftp.gnu.org/gnu/libidn/libidn2-2.3.8.tar.gz
- sha256 del tarball (el mismo del PKGBUILD de MSYS2):
  `f557911bf6171621e1f72ff35f5b1825bb35b52ed45325dcdee931e5d3c0787a`

Copiados sin cambios de la raíz del tarball:

| Archivo | Qué es | Por qué va |
|---|---|---|
| `COPYINGv2` | GPL versión 2 | La biblioteca es GPL-2.0-or-later o LGPL-3.0-or-later |
| `COPYING.LESSERv3` | LGPL versión 3 | La otra opción de la biblioteca |
| `COPYING` | GPL versión 3 | La LGPL v3 la incorpora |
| `COPYING.unicode` | Licencia de los datos de Unicode | Según el README de libidn2, los datos de Unicode quedan dentro de la biblioteca ("parsed into static storage in the library") |

Si MSYS2 actualiza libidn2 a otra versión, el script avisa (sin fallar):
revisa los textos del nuevo tarball y actualiza esta carpeta y la línea
"Versión".
