# ARKANOID — Core de Vídeo (vc) para 6502 / Tang Nano 9K

El clásico rompeladrillos para el computador 6502 con el **Core de Vídeo**
(`videocore-6502-cc65`). Programado en C con cc65 y la biblioteca `vc`.

> **Origen:** este proyecto sigue la misma estructura que `snake/`, `Tetris/`,
> `Pacman/` y `SpaceInvaders/`: un proyecto **autónomo** con su propia lógica,
> configuración y copia de la biblioteca (`lib/vc.lib`, `config/programa.cfg`,
> `include/video.h`).
>
> 📌 ¿Vas a empezar otro juego? Lee antes
> [`doc/TIPS-Y-TRAMPA.md`](doc/TIPS-Y-TRAMPA.md) (copia de la guía general del
> Core de Vídeo: tiles, sprites, flips, paletas, texto, SID, depuración).

---

## Cómo jugar

Se puede jugar con **joystick Atari (DB9)** o con **teclado UART**, en paralelo.

| Acción | Joystick | Teclado UART |
|--------|----------|--------------|
| Mover pala a la izquierda | stick ← | `a` / `4` / flecha ← |
| Mover pala a la derecha   | stick → | `d` / `6` / flecha → |
| Lanzar la bola            | botón   | `ESPACIO` / `ENTER` |
| Salir                     | —       | `q` |

La bola arranca **pegada a la pala**; pulsa el botón (o ESPACIO) para lanzarla.
Si no la lanzas, sale sola pasados unos segundos.

### Reglas

- **Objetivo:** romper todos los ladrillos del nivel para pasar al siguiente.
- **Vidas:** empiezas con **3**. Cada vez que la bola cae por abajo pierdes una.
- **Vidas a 0 → GAME OVER.** Pulsa el botón (o `R`) para reiniciar, `Q` para salir.
- **Puntuación:** 10 puntos por ladrillo.
- **Niveles:** al limpiar el tablero subes de nivel (hasta 9) y la bola **acelera**.
- **Ángulo:** el rebote en la pala cambia según el **punto de golpe**, que se
divide en **4 zonas**: las dos puntas lanzan a 45° hacia su lado y las dos zonas
  centrales a 25°. Así la bola se puede **dirigir**, como en el Arkanoid arcade.

---

## Diseño del juego (decisiones fijadas)

### Los ladrillos son **tiles** del tilemap; la pala y la bola son **sprites**

El hardware dibuja con un motor de tiles + sprites (sin framebuffer), con un
límite de **8 sprites por línea**. La rejilla de ladrillos tiene 13 ladrillos por
fila → no caben como sprites. Por eso:

- **Ladrillos → tiles** del tilemap (`vc_put_cell`), cada ladrillo de **2 tiles**
  de ancho (como el original).
- **Pala (24×8 normal) y bola (6×6) → sprites** (OAM 0/1/11/15 para los trozos de
  la pala, OAM 2/12/13 para las bolas): son pocos, móviles y con movimiento fino
  píxel a píxel. La bola se dibuja dentro de un tile de 8×8 (círculo de 6×6
  centrado, desplazado 1 px), pero su **hitbox es de 6×6** para que coincida con lo
  visible. La bola lleva un **bisel tipo esfera** (3 tonos de gris: blanco
  arriba-izq., gris medio, gris oscuro abajo-der.) para dar volumen 3D.

### El estado vive en RAM, no se lee la VRAM

La biblioteca `vc` **no expone lectura del tilemap**. Igual que `Tetris` con su
array `board[]`, Arkanoid guarda el estado de los ladrillos en
`brick[filas][ladrillos]` (5×13) y el tilemap solo lo refleja. La colisión de la
bola es **en software** contra ese array. Cada ladrillo = 2 celdas: al romperlo se
borran **ambas** celdas de golpe (un impacto rompe el ladrillo entero).

### Física de la bola (velocidad por eje + colisión por borde)

La bola lleva la posición con 8 bits enteros + 8 de fracción (Q8.8) en `uint16_t`
y la velocidad **por eje** en `vx`/`vy`, también Q8.8 pero **con signo**: el signo
es la dirección y el valor la rapidez. Al no compartir una única velocidad, el
**ángulo no está limitado a 45°**: cada eje lleva la suya y el rebote en la pala
puede salir a 25°, 45° o recto, según la zona golpeada.

La velocidad del eje Y (`ball_vy`) sube con el nivel y **no cambia al rebotar en
la pala**: solo se reescribe `vx` según la zona. Así el ángulo se mantiene a
cualquier velocidad.

Cada frame se avanza el número de **pasos de 1 píxel** que indique cada eje (el
bucle corre tantas veces como el eje más rápido; el otro solo avanza mientras le
queden pasos). En cada paso se prueba el **borde delantero** de la bola (no solo
el centro, para que una bola pequeña no se cuele en el muro), por separado:

- el borde **en X** → rebote en X (rompe ladrillo o rebota en el muro),
- el borde **en Y** (en los dos bordes horizontales) → rebote en Y.

Así las esquinas se resuelven solas (rebotan los dos ejes), y un **clamp** final
garantiza que la bola nunca abandona el campo por los lados ni por arriba. Es la
técnica clásica de breakout, adaptada al sprite de 8×8.

> ⚠️ **Cuidado con el Q8.8 y el borde derecho.** La posición va en `uint16_t`, que
> solo representa hasta `0xFFxx` (255.996). El borde derecho de la bola **no puede**
> llegar a 256: `256 << 8` da `0x10000`, que **desborda a 0** en `uint16_t` y la
> bola saltaría al lado izquierdo. Por eso el muro derecho está en la columna 32
> (píxel 255 es el máximo interior) y `BALL_X_MAX = 250`.
> `tools/test_ball.py` simula la física en el PC con las **velocidades por eje**
> que genera el juego (45°, 25° y recto, en los 9 niveles) y comprueba que
> `ball_x` se queda siempre en `[64, 250]`.

### Presupuesto de recursos

| Recurso | Límite | Uso |
|---|---|---|
| Sprites (OAM) | 32 (máx. **8 por línea**) | pala 4 + 3 bolas + 2 cápsulas + 2 rayos |
| Patrones de fondo | 256 (fuente en `$20`-`$7F`) | 4 (muro + 2 mitad de ladrillo + fuente) |
| Patrones de sprite | 64 | 15 (pala ×3, bola, cápsulas ×8, rayo ×2 usos) |
| Ladrillos | 13×5 = 65 (2 celdas c/u) | array en RAM (65 bytes) |
| RAM | `$0800-$3EFF` (~13.75 KB) | `BSS` acaba en `$3E40` → **~190 bytes libres** |

> ⚠️ La RAM del proyecto va **ajustada**. Cualquier cosa nueva hay que "pagarla"
> quitando otra: medir siempre con `python tools/fn_sizes.py build/game.lst`
> (tamaño por función) y con `make map`. Mitigaciones que ya se usaron aquí:
>
> - Desenrollar tablas `const` a código; usar literales en `put_str_pal` (evita
>   arrastrar `memcpy`); sustituir `%` por divisiones con constante (evita
>   `udiv`/`umod` de 16 bits); compactar bucles.

### Prueba de la física (PC)

`tools/test_ball.py` reproduce en Python la aritmética Q8.8 de `ball_step()` y
simula miles de frames con las **velocidades por eje** que produce el juego
(rebote en las puntas = 45°, rebote suave = 25°, esquina = recto) en los 9
niveles, verificando que la bola nunca sale del campo. Útil porque el juego no se
puede ejecutar sin hardware:

```bash
python tools/test_ball.py
```

### Patrones y color (arte propio del juego)

Los patrones son **arte original** convertido de Piskel con
`tools/piskel2planes.py`:

- **Ladrillo = dos tiles de 8×8** (`TILE_BRICK_L` + `TILE_BRICK_R`) que juntos
  forman un ladrillo de **16×8** con bisel 3D.
- **Pala**: la barra se compone de trozos de 8 px. Las **puntas** llevan el bisel
  de los extremos y todo lo de en medio es **relleno** (`PADDLE_PAT_MID`), así
  que cualquier ancho forma una barra continua:

  | Estado | Ancho | Sprites | Composición |
  |---|---|---|---|
  | Normal (por defecto) | 24 px | 3 | `[L][MID][R]` |
  | Encogida (cápsula **N**) | 16 px | 2 | `[L][R]` |
  | Extendida (cápsula **E**) | 32 px | 4 | `[L][MID][MID][R]` |

  La punta **derecha no tiene patrón propio**: es la izquierda espejada con
  `VC_SPR_FLIP_X` (el core voltea sprites de forma fiable), lo que ahorra
  32 bytes de RODATA. Con el efecto **láser** las puntas usan su patrón con cañón.
- **Bola**: círculo 6×6 dentro del tile 8×8, con bisel de 3 grises.
- **Muro / HUD / texto**: paleta 0, índice 3 = blanco.
- La paleta 3 **no** se toca: su color 3 es la entrada 15 = `BG_COLOR`.

…y cada píxel usa uno de **3 índices** de paleta (0 = transparente), donde el
color real lo pone la **paleta de la celda**:

| Índice | Zona del bisel | Color (ladrillo, arte del usuario) |
|---|---|---|
| 3 | claro (highlight arriba-izq.) | `0xfff8a7a6` |
| 2 | medio (relleno) | `0xffef6e6d` |
| 1 | oscuro (sombra abajo-der.) | `0xffb10a09` |

- **Un mismo patrón** de ladrillo se ve de **distinto color según la fila**. Hay
  **5 filas** con un degradado **simétrico** de 3 colores (solo caben 3 paletas):
  - **fila 1 ROJO** (pal 0) → **fila 2 NARANJA** (pal 1) → **fila 3 AMARILLO** (pal 2)
    → **fila 4 NARANJA** (pal 1) → **fila 5 ROJO** (pal 0)
  - Cada paleta = una escala de brillo (claro/medio/oscuro) del mismo tono.
- **Pala**: **24×8 px por defecto** (3 trozos de 8), azul.
- **Muro / HUD / texto**: paleta 0, índice 3 = blanco.
- La paleta 3 **no** se toca: su color 3 es la entrada 15 = `BG_COLOR`.

> El core solo tiene **4 paletas de fondo** y una (la 3) es `BG_COLOR`, así que
> quedan **3 usables**. No se pueden cambiar por banda de raster (las bandas del
> core solo afectan al **scroll**), así que el máximo es **3 colores**; las 5 filas
> los repiten en degradado simétrico. La fila ROJA comparte la paleta 0 con el
> muro: el índice 3 de esa paleta es blanco y hace de highlight del ladrillo rojo.

### Efectos (power-ups)

Al romper un bloque puede caer una **cápsula** (1 de cada 3). Al recogerla con la
pala se aplica su efecto (o penalización). Las **8** funcionan:

| Letra | Efecto | Estado |
|---|---|---|
| **E** | pala se ensancha (24 → 32 px) | ✅ |
| **N** | pala se encoge (24 → 16 px, penalización) | ✅ |
| **S** | bola lenta | ✅ |
| **B** | la bola atraviesa bloques | ✅ |
| **C** | catch: la pala atrapa la bola | ✅ |
| **P** | vida extra | ✅ |
| **D** | disruption (triplica la bola: 3 bolas) | ✅ |
| **L** | laser (la pala dispara rayos por las dos puntas) | ✅ (2 rayos; con la pala extendida solo 1) |

Las cápsulas caen como sprites; buenas = verde, malas (N) = rojo. Los efectos
temporales duran ~10 s (`FX_TIME`). Ver `doc/ARQUITECTURA-ARKANOID.md`.

> La tabla de patrones de las cápsulas y el reparto de los power-ups es uniforme
> (`1 + rng % 8`), así que **las 8 letras salen con la misma probabilidad**. Si
> tienes la impresión de que "no caen rojas (N)" es solo mala suerte: la N es 1 de
> 8. Para verla antes, sube `CAPS_CHANCE` a 2 (caen más cápsulas).

### Flujo de pantallas

```
TITULO ──(BOTON)──> JUEGO ──nivel superado──> JUEGO (nivel+1, bola más rápida)
                        │
                        └──sin vidas──> GAME OVER ──(BOTON/R)──> JUEGO
                                            │
                                            └──(Q)──> DESPEDIDA ──> monitor
```

Durante el juego, **Q** sale al monitor.

### Sonido (SID 6581, compatible C64)

Driver en `sound.c` / `include/sound.h`. Voces:

| Voz | Uso |
|-----|-----|
| 1 | rebote (pared/pala) **y** romper un ladrillo (blip con sweep) |
| 2 | **no se usa** — la voz 2 está muda en este core del FPGA |
| 3 | subir de nivel / bono **y** perder una vida (ruido) |

> ⚠️ **La voz 2 del SID no suena en este core** (probado: los mismos efectos en
> la voz 1 sí suenan). Por eso todos los efectos van por las voces 1 y 3.

`snd_update()` se llama una vez por frame. La **voz 3** (oscilador de ruido) se
usa también como fuente de aleatoriedad (`snd_random()`, registro `$D41B`) para
sembrar el RNG (evita que el nivel salga siempre igual).

### Entrada: joystick Atari (DB9) + teclado UART

Driver en `joy.c` / `include/joy.h` (copiado del proyecto `snake`). El joystick y
el teclado funcionan **en paralelo**; el juego solo lee un bitmask de acciones
(`JOY_A_*`) en `read_input()`.

---

## Compilar

```bash
make CC65_HOME=D:/cc65
```

Genera `output/game.bin`. Cárgalo en el monitor:

```
LOAD GAME 0800
R 0800
```

> **Nota:** cc65 es un ejecutable de Windows (`D:/cc65`). Compila desde **Git
> Bash / cmd**, no desde WSL (no se pueden lanzar los `.exe`).

---

## Estructura

```
Arkanoid/
├── makefile            build autónomo (ROOT = .)
├── game.c              lógica del juego
├── sound.c             driver SID
├── joy.c               driver del joystick Atari (GPIO)
├── startup.s           arranque cc65 -> jmp $8000 (monitor)
├── include/
│   ├── video.h         API del Core de Vídeo (copia de la lib)
│   ├── sound.h         API del driver de sonido
│   ├── joy.h           API del driver del joystick
│   └── romapi.h        ROM API del monitor
├── config/
│   └── programa.cfg    config del linker (copia de la lib)
├── doc/
│   ├── ARQUITECTURA-ARKANOID.md   diseño detallado del juego
│   ├── VIDEO-LIB.md               manual de la biblioteca
│   └── TIPS-Y-TRAMPA.md           lecciones y trampas (para empezar otro juego)
├── lib/
│   └── vc.lib          copia de la biblioteca
├── build/              objetos
└── output/
    ├── game.bin        binario final
    └── game.map        mapa de memoria
```

### Actualizar la biblioteca

```bash
cp ../videocore-6502-cc65/output/vc.lib   lib/vc.lib
cp ../videocore-6502-cc65/src/video.h     include/video.h
cp ../videocore-6502-cc65/config/programa.cfg config/programa.cfg
```
