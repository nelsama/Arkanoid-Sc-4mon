# Arquitectura de ARKANOID (Core de Vídeo 6502)

Documento de diseño: describe **cómo** está construido `game.c` y por qué se
tomaron esas decisiones. Para el uso general del Core de Vídeo, ver
[`VIDEO-LIB.md`](VIDEO-LIB.md) y [`TIPS-Y-TRAMPA.md`](TIPS-Y-TRAMPA.md).

---

## 1. El problema: dibujar un Arkanoid sin framebuffer

El Core de Vídeo es un coprocesador gráfico estilo NES/VIC-II: **tiles + sprites,
sin framebuffer**. El CPU no pinta píxeles; escribe VRAM y registros.

Restricciones que condicionan el diseño:

- **32 sprites** en total, y **máximo 8 por línea** (si no, `OVERFLOW`).
- **256 patrones de fondo**, pero `$20-$7F` los ocupa la fuente ASCII.
- **64 patrones de sprite**.
- Toda escritura a VRAM/OAM debe hacerse **dentro del VBLANK**.
- No hay lectura del tilemap desde la biblioteca.

Un Arkanoid clásico necesita:

- Una **rejilla de ladrillos** (aquí 13×5 = 65 ladrillos, cada uno de **2 celdas**
  de ancho = 130 celdas): demasiados para sprites, y con >8 por línea. → **tiles**.
- Una **pala** que se mueve pixel a pixel.
- Una **bola** que se mueve pixel a pixel y rebota.

---

## 2. Reparto de la pantalla (40×30 celdas)

```
fila 1        : HUD  (PTS / NIV / VIDAS)   <- se usa la fila 1, no la 0
fila 3..27    : campo de juego, enmarcado por MUROS (tiles)
  columnas 7..32  -> muro izquierdo en 7, muro derecho en 32
  filas 4..8      -> rejilla de ladrillos (13 ladrillos x 2 celdas = 26 celdas)
  fila 27         -> suelo (muro visual; la bola NO rebota, se pierde la vida)
  Y = 206         -> pala (PADDLE_Y)
filas 0,2,28,29 : margen
```

> **Por que la fila 1 para el HUD:** la fila 0 se ve **desplazada** por el
> pipeline del core y la 29 puede **recortarse** por overscan (ver
> `TIPS-Y-TRAMPA.md` §4.3). Todo el contenido vive en las filas seguras 1..28.
>
> **Por que el marco esta en la columna 6 y no en la 0:** la columna 0 tambien
> se recorta/desplaza; centrando el campo el marco queda claramente visible.

- **Muros** (arriba, izquierda, derecha y suelo): tiles `TILE_WALL`, color 3 de
  la paleta 0 (= blanco).
- **Suelo**: es un muro *visual*, pero **la bola no rebota en él**: si llega
  abajo, se pierde una vida. Por eso `solid_px()` no lo considera sólido.
- Coordenadas del interior jugable (en píxeles):

```
PLAY_X_MIN = 64     PLAY_X_MAX = 255
PLAY_Y_MIN = 32     PLAY_Y_MAX = 215   (más allá = vida perdida)

ball_x (esquina izq. de la bola) va de 64 a 248 (= BALL_X_MAX)
```

> ⚠️ **Límite derecho y Q8.8.** La posición de la bola es Q8.8 en un `uint16_t`,
> que solo representa hasta 255.996 (`0xFFxx`). El interior **no puede** llegar a
> la columna 33 (píxel 264) porque el borde de la bola alcanzaría 256, y
> `256 << 8 = 0x10000` **desborda `uint16_t` → 0**: la bola saltaría al lado
> izquierdo. Por eso el muro derecho está en la **columna 32** (píxel 255 es el
> máximo interior). Ver `tools/test_ball.py`, que simula la física y comprueba
> que `ball_x` nunca sale de `[64, 248]`.

---

## 3. Ladrillos: tiles + estado en RAM

El estado es `brick[BRICK_ROWS][BRICK_COLS]` (5×13 = 65 ladrillos). `0` = vacío;
si no, `fila+1` (el "tipo", que de momento solo decide el color).

**Cada ladrillo ocupa 2 celdas de ancho (16×8 px)**, como en el Arkanoid original.
`brick_paint`/`brick_erase` escriben o borran las **dos** celdas del ladrillo, y
`brick_at`/`brick_break` convierten la columna de celda a índice de ladrillo con
`(col - BRICK_X0) / BRICK_W`. Así, cuando la bola golpea **cualquiera** de las dos
celdas, se rompe el ladrillo entero (dos tiles de golpe).

- `bricks_init()` rellena el array y **pinta** los ladrillos con `vc_put_cell`.
- `brick_at(col,row)` consulta el array (0 = libre).
- `brick_break(col,row)` pone a 0 y **borra** el tile (escribe `TILE_BLANK`).

Ventaja: la colisión nunca lee VRAM; solo un array en RAM, barato para el 6502.
La VRAM es una "vista" que se mantiene sincronizada.

El patrón `tile_brick` usa dos planos: índice 3 en el borde (color principal) e
índice 1 en el interior (sombra). Así una **misma** forma se ve naranja (paleta 1)
o cian (paleta 2) según la **paleta de la celda**, sin cambiar el patrón.

---

## 4. Pala y bola: 3 sprites

| Sprite (OAM) | Objeto | Patrón |
|---|---|---|
| 0 | pala, mitad izquierda | `spr_paddle_l_*` |
| 1 | pala, mitad derecha   | `spr_paddle_r_*` |
| 2 | bola (círculo 4×4)    | `spr_ball_*` |

La pala son dos sprites de 8×8 contiguos en X (16 px en total). Se **redibujan
cada frame dentro del VBLANK** (`paddle_draw()`, `ball_draw()`): si no, podrían
perderse o salir partidos (ver §3.4 de las trampas).

---

## 5. Física de la bola

### Representación

- Posición: `ball_x`, `ball_y` en **Q8.8** dentro de `uint16_t` (parte entera en
  el byte alto). Evita el Q8.8 con signo, que en cc65 produce avisos de
  desbordamiento con literales como `0xFF00`.
- Dirección: `ball_dx` (1 = derecha, 0 = izquierda), `ball_dy` (1 = abajo,
  0 = arriba).
- Velocidad: `ball_speed` = **magnitud** Q8.8 por eje. `ball_set_speed()` la fija
  según el nivel: `1.00 px/frame` en el nivel 1, `+0.25` por nivel (tope 3.00).

### Avance por pasos de 1 píxel (colisión por **borde**) —

Un sprite de 8×8 puede solapar **dos celdas a la vez**. Si se comprobase solo el
**centro** de la bola, esta se metería en el muro antes de rebotar y parecería
"salirse por el margen". Por eso se comprueba el **borde delantero** de la bola en
la dirección del movimiento (y, en vertical, los **dos** bordes horizontales, para
no colarse entre dos ladrillos):

```
steps = ball_speed >> 8   (+1 si la fraccion acumulada llega a 0x100)

repetir 'steps' veces:
    x,y = esquina sup-izq de la bola
    lx = x ; rx = x+7 ; ty = y ; byy = y+7    (bordes)
    cx = x+4 ; cy = y+4                        (centro)

    # Eje X: mira el borde delantero (rx si va a la derecha, lx si a la izq.)
    si SOLIDO(borde_delantero_x, cy):
        romper ladrillo / sonido rebote ; dx = !dx

    # Eje Y: mira el borde delantero en AMBOS bordes horizontales
    si SOLIDO(lx, borde_delantero_y) o SOLIDO(rx, borde_delantero_y):
        romper ladrillo / sonido rebote ; dy = !dy

    si dy (bajando) y solapa la pala (AABB estricto):
        dy = 0 ; dx = segun el punto de golpe ; break

    aplicar movimiento (±1 px)
    CLAMP: si la bola salio del campo por los lados/arriba, se reposiciona
    si fuera por abajo -> fin (vida perdida)
```

- `solid_px(px,py)` es **sólida** si es el muro izquierdo/derecho, el techo, o un
  ladrillo.
- La comprobación **separada por ejes** resuelve las esquinas: si toca un ladrillo
  en diagonal, rebota en X y en Y, y lo rompe.
- El **clamp** final es una red de seguridad: pase lo que pase en la detección,
  la bola nunca abandona el campo por los lados ni por arriba.

### Ángulo al golpear la pala

Se calcula la posición relativa del centro de la bola respecto al centro de la
pala. Borde izquierdo → sale hacia la izquierda; borde derecho → a la derecha. Un
golpe muy centrado sale en un ángulo al azar (evita que la bola se quede
"vertical" rebotando sola sin salir nunca).

---

## 6. Flujo de juego

```
main()
  setup_video()          # wait_ready -> vblank -> clear_vram -> fondo/paletas/patrones
  snd_init(); joy_init()
  title_screen()         # espera botón; si 'q' -> bye_screen()
  do {
      rng_seed(); score=0; level=1; lives=3
      field_draw(); hud_draw()
      while (!quit) {
          play_level()   # una partida de un nivel
          si nivel superado -> level++ (hasta 9) + bono + pausa
          si no -> GAME OVER (o salida)
      }
      panel GAME OVER: botón/R = otra partida, Q = salir
  } while (again)
  bye_screen()           # limpieza + jmp $8000 (monitor)
```

### Estados dentro de `play_level()`

- `ball_stuck` = 1: la bola sigue pegada a la pala (se mueve con ella). Se lanza
  con botón/ESPACIO o sola tras `AUTOLAUNCH_FRAMES`.
- `bricks_left` = 0 → nivel superado (`level_clear`).
- La bola cae por abajo → `lives--`; si llega a 0 → `game_over`, si no, se
  recoloca con `paddle_reset()`.

---

## 7. Salida limpia al monitor

Al terminar (o al salir con `q`), `main()` deja el hardware en un estado
predecible antes del `jmp $8000`:

```c
bye_screen();
snd_silence();
vc_wait_vblank();
vc_clear_oam();       /* apaga todos los sprites */
vc_set_scroll_x(0);
vc_set_scroll_y(0);
```

`startup.s` hace `jsr _main` y, al volver, `jmp $8000` (reinicia la RAM del
monitor).

---

## 8. Encaje en la memoria

Tras compilar, el mapa de memoria (`output/game.map`) confirma que todo cabe:

| Segmento | Fin | Límite |
|---|---|---|
| CODE+RODATA+DATA+BSS | `$2B16` | `$3DFF` |

Sobra más de 4 KB de RAM, margen de sobra para más niveles o efectos.

---

## 9. Posibles mejoras

- **Ladrillos de varias durezas** (2 golpes): ya hay sitio en el array (`tipo+1`);
  bastaría decrementar el tipo y cambiar la paleta en vez de borrar.
- **Power-ups** (pala ancha, multi-bola): sprites adicionales; ojo con el límite
  de 8/línea.
- **Fondo con scroll**: el core lo soporta (bandas), pero aquí no aporta.
- **Highscores en SD**: la RAM es de sesión; usar `mfs_create`/`mfs_write` para
  persistir.
