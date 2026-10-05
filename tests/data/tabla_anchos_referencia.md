# Referencia del reparto de tablas

`tabla_anchos_referencia.txt` contiene 513 resultados: índice de tabla, ancho de pantalla, número de columnas y ancho de cada columna. El orden es `tabla_reparto.md`, `tabla_angosta.md` y `chatbot_test::table_10x50()`, cada una de 30 a 200 columnas inclusive. Cero columnas significa que se dibuja como fichas.

Se generó desde `markdown_view.cpp` de `main` (`69f5858`) en una copia temporal compilada con GCC 16, `-O3 -DNDEBUG`. La caché pedida en el punto 6 ya estaba implementada desde `0af0b9a`; por eso la referencia sin caché es una reconstrucción, no una revisión anterior del repositorio.

La copia conservó el parseo, las medidas, la bisección, los pisos y los desempates originales. Solo sustituyó el paso 2: para medir el alto de un candidato, recuenta todas sus celdas con `count_lines` y suma el máximo de cada fila. En cada vuelta mide el vector actual, y después cada candidato con una columna adicional. Elige la mayor reducción de alto; en empate, el mayor ancho pendiente hasta el natural; si también empata, la primera columna. Incrementa solo la ganadora y repite.

Se compararon los 513 resultados de esa copia con el algoritmo con caché mediante `cmp` antes de incorporarlos. La prueba `[reparto]` conserva esa comparación en la suite de vista, con y sin sanitizadores. La tabla grande usa el mismo generador que la prueba de tiempo para evitar diferencias entre las muestras.
