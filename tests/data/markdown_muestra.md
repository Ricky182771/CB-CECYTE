# Muestra de markdown

Párrafo con **negritas**, *cursivas*, ***ambas***, ~~tachado~~, ==resaltado==,
`código en línea` y un [enlace](https://example.com/docs). Autolink: https://example.org.
Con salto duro al final  
de esta línea. Entidades: &copy; &mdash; &euro; &#233; &desconocida;

## Listas

- Elemento con un texto lo bastante largo para que se ajuste en varias líneas.
  - Anidado
    - Tercer nivel
- [ ] Tarea pendiente
- [x] Tarea hecha

3. Empieza en tres
4. Sigue en cuatro

### Citas y alertas

> Una cita.
>
> > Una cita dentro de otra.

> [!NOTE]
> Una nota.

> [!TIP]
> Un consejo.

> [!IMPORTANT]
> Algo importante.

> [!WARNING]
> Una advertencia.

> [!CAUTION]
> Precaución.

#### Código

```cpp
#include <iostream>

int main() {
    std::cout << "Una línea de código bastante larga que no cabe en cuarenta columnas" << '\n';
    return 0;
}
```

```
sin lenguaje
	con tabulador
```

---

##### Tablas

| Nombre | Edad | Ciudad |
|:-------|-----:|:------:|
| Ana    |   30 | León   |
| Bo     |    5 | Lima   |

| Columna | Descripción larga | Otra columna | Una más | Y otra |
|---|---|---|---|---|
| uno | Un texto bastante largo que tendrá que ajustarse dentro de la celda | dos | tres | cuatro |

###### Unicode y control

Emoji 😀🎉 y CJK: 漢字かな交じり文 ここに長い日本語の文章があります。

| 名前 | 都市 | 😀 |
|---|---|---|
| 山田 | 東京 | sí |

Secuencia ESC: [31mesto no debe salir en rojo[0m y campana .

Imagen: ![logo del proyecto](https://example.com/logo.png)

Matemáticas: $E = mc^2$ y $$\int_0^1 x\,dx$$

Una nota al pie[^1] y otra[^larga].

[^1]: Texto de la primera nota.
[^larga]: Una nota más larga que también tiene que ajustarse al ancho disponible.
