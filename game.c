/**
 * ============================================================================
 * game.c - ARKANOID para el Core de Video (vc) / Monitor 6502
 * ============================================================================
 * Clasico rompeladrillos:
 *   - Mueves la PALA abajo con el joystick (o A/D) y la BOLA rebota sola.
 *   - Rompe todos los LADRILLOS para pasar de nivel; cada nivel la bola va mas
 *     rapido. Tienes 3 vidas.
 *   - Si la bola cae por abajo, pierdes una vida. Al quedarte sin vidas: GAME OVER.
 *
 * TECNICA DE DIBUJO (igual que en SNAKE/TETRIS)
 *   El core dibuja con tiles + sprites (sin framebuffer). Los LADRILLOS son una
 *   rejilla, y el core no deja mas de 8 sprites por linea; ademas cada linea del
 *   campo tendria ~13 ladrillos. Por eso los ladrillos son TILES del tilemap.
 *   La PALA y la BOLA son los 2 unicos SPRITES (OAM 0 y 1): necesitan movimiento
 *   fino pixel a pixel y no son rejilla.
 *
 *   El estado de los ladrillos vive en un array en RAM (brick[filas][cols]); el
 *   tilemap solo lo refleja. La colision de la bola es en SOFTWARE contra ese
 *   array (no se lee la VRAM: la lib no expone lectura del tilemap).
 *
 * FISICA
 *   La bola lleva posicion con 8 bits enteros + 8 de fraccion por eje (Q8.8)
 *   y velocidad (vx,vy) en subpixeles/frame. Cada frame se avanza en pasos de 1
 *   pixel; en cada paso se comprueba la celda destino:
 *     - pared/ladrillo: rebota el eje correspondiente y rompe el ladrillo.
 *     - pala: rebota hacia arriba, con angulo segun donde golpee.
 *
 * CONTROLES (UART):
 *   a/4/flecha izq = mover pala a la izquierda
 *   d/6/flecha der = mover pala a la derecha
 *   ESPACIO/ENTER  = lanzar la bola (y empezar)
 *   q              = salir.  En GAME OVER: r = jugar de nuevo, q = salir.
 * ============================================================================
 */

#include <stdint.h>
#include "video.h"
#include "romapi.h"
#include "sound.h"
#include "joy.h"

/* ===========================================================================
 * GEOMETRIA DE LA PANTALLA (40x30 celdas de 8x8 = 320x240 px)
 * ===========================================================================
 * HUD arriba (fila 0), campo de juego enmarcado por muros, y una zona de HUD
 * inferior. El campo se define en CELDAS para los ladrillos y en PIXELES para
 * la pala/bola (movimiento fino).
 * =========================================================================== */
#define SCREEN_ROWS   VC_SCREEN_ROWS   /* 30 */
#define SCREEN_COLS   VC_SCREEN_COLS   /* 40 */

/* Marco del campo (en celdas): el muro ocupa estas celdas.
 * Se mantiene dentro de las filas seguras 1..28 (la fila 0 se ve desplazada por
 * el pipeline y la 29 puede recortarse por overscan). El campo es un rectangulo
 * CENTRADO (margen de 7 columnas por lado).
 *
 * OJO con el limite derecho: la posicion de la bola es Q8.8 en un uint16_t, que
 * solo llega hasta 255.996 (0xFFxx). Por eso el interior NO puede pasar de la
 * columna 32 (pixel 255): si el borde de la bola llegase a 256, su Q8.8
 * (256<<8 = 0x10000) desbordaria a 0 y la bola saltaria al lado izquierdo. */
#define FIELD_TOP     3                /* fila del muro superior            */
#define FIELD_BOTTOM  27               /* fila del muro inferior (suelo)    */
#define FIELD_LEFT    7                /* columna del muro izquierdo        */
#define FIELD_RIGHT   32               /* columna del muro derecho          */
#define FIELD_COLS    (FIELD_RIGHT - FIELD_LEFT + 1)   /* 26 */
#define FIELD_ROWS    (FIELD_BOTTOM - FIELD_TOP + 1)   /* 25 */

/* Limites en PIXELES del interior del campo (donde rebota la bola). */
#define PLAY_X_MIN    ((FIELD_LEFT  + 1) * 8)          /* 64 */
#define PLAY_X_MAX    ((FIELD_RIGHT)     * 8 - 1)      /* 255 (borde interior der.) */
#define PLAY_Y_MIN    ((FIELD_TOP   + 1) * 8)          /* 32 */
#define PLAY_Y_MAX    ((FIELD_BOTTOM)    * 8 - 1)      /* 215 (suelo) */

#define BALL_SIZE     6                /* lado de la bola (hitbox) en pixeles */

/* La bola se dibuja como un circulo de 6x6 centrado dentro del tile de sprite
 * (8x8), dejando 1 px de margen. El sprite se coloca en (x-1, y-1) para que el
 * borde VISUAL coincida con el hitbox de BALL_SIZE. Ver ball_draw(). */
#define BALL_OFF      (-1)             /* offset de dibujo del sprite de bola */

/* Posicion entera MAXIMA de ball_x (esquina izq. de la bola) para que su borde
 * derecho quede dentro del campo Y quepa en Q8.8. */
#define BALL_X_MAX    (PLAY_X_MAX - BALL_SIZE + 1)     /* 250 */

/* ===========================================================================
 * REJILLA DE LADRILLOS
 * ===========================================================================
 * Cada LADRILLO ocupa 2 celdas (16x16 px) de ancho y 1 de alto, como en el
 * Arkanoid original. Los ladrillos LLENAN el ancho interior del campo:
 *   BRICK_COLS = 13 ladrillos -> 26 celdas (todo el interior entre muros).
 * El estado se guarda por LADRILLO (brick[fila][ladrillo]); al romper uno se
 * borran sus DOS celdas del tilemap.
 * =========================================================================== */
#define BRICK_W       2                 /* celdas de ancho por ladrillo */
#define BRICK_COLS    ((FIELD_COLS - 2) / BRICK_W)  /* 13 ladrillos por fila */
#define BRICK_ROWS    5                 /* 5 filas (3 colores, repetidos en degradado) */
#define BRICK_X0      (FIELD_LEFT + 1)  /* 7: primera celda interior   */
#define BRICK_Y0      6                 /* primera fila del ladrillo: deja 2 filas
                                         * (16 px) de hueco bajo el techo para que
                                         * la bola pueda pasar POR ENCIMA de los
                                         * bloques y rebotar con el techo. */
#define BRICK_COUNT   (BRICK_COLS * BRICK_ROWS)   /* 65 ladrillos */

/* ===========================================================================
 * PATRONES (tiles de fondo y sprites)
 * ===========================================================================
 * color del pixel = (plano1 << 1) | plano0   (indices 0..3, 0 = transparente).
 * =========================================================================== */

/* --- Tiles propios (fuera del rango de la fuente $20-$7F) --- */
#define TILE_BLANK     0x00   /* transparente (fondo negro)                  */
#define TILE_WALL      0x01   /* muro del campo (bloque solido, color 3)     */
#define TILE_BRICK_L   0x02   /* ladrillo, mitad IZQUIERDA (8x8)             */
#define TILE_BRICK_R   0x03   /* ladrillo, mitad DERECHA   (8x8)             */

/* Muro: bloque solido (indice 3, usa el color 3 de la paleta). */
static const uint8_t tile_wall_p0[8] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
};

/* ---------------------------------------------------------------------------
 * LADRILLO (16x8 = dos tiles de 8x8: mitad izq. + mitad der.).
 * Arte original del usuario (formato Piskel), con bisel 3D:
 *   0xfff8a7a6 = CLARO  (bisel superior / highlight) -> indice 3
 *   0xffef6e6d = MEDIO  (relleno)                     -> indice 2
 *   0xffb10a09 = OSCURO (bisel inferior / sombra)      -> indice 1
 * El color real de cada indice lo pone la PALETA de la celda (una por fila).
 * Los planos se generaron con: idx = (plano1<<1)|plano0.
 * --------------------------------------------------------------------------- */
/* Mitad IZQUIERDA */
static const uint8_t tile_brick_l_p0[8] = {
    0xFF, 0xFF, 0xC0, 0xC0, 0xC0, 0xC0, 0xFF, 0xFF
};
static const uint8_t tile_brick_l_p1[8] = {
    0x7F, 0x3F, 0x3F, 0x3F, 0x3F, 0x3F, 0x40, 0x80
};
/* Mitad DERECHA */
static const uint8_t tile_brick_r_p0[8] = {
    0xFF, 0xFF, 0x03, 0x03, 0x03, 0x03, 0xFF, 0xFF
};
static const uint8_t tile_brick_r_p1[8] = {
    0xFE, 0xFD, 0xFF, 0xFF, 0xFF, 0xFF, 0x03, 0x01
};

/* --- Sprite de la PALA (16x8, se dibuja como 2 sprites 8x8 lado a lado).
 * Patrones propios de la pala (arte del usuario). Estructura del bisel:
 *   indice 3 = claro  (borde superior/highlight)  -> 0xfff8a7a6
 *   indice 2 = medio  (relleno central)           -> 0xffef6e6d
 *   indice 1 = oscuro (detalles laterales)        -> 0xffb10a09
 * Los planos se generaron de la mascara de zonas del usuario
 * (gris=relleno idx2, amarillo=borde idx3, verde=detalle idx1;
 *  idx = (plano1<<1)|plano0). --- */
#define PADDLE_PAT_L 0
/* El trozo derecho NO tiene patron propio: es el IZQUIERDO espejado con
 * VC_SPR_FLIP_X (verificado: los planos son exactamente el espejo). Asi el
 * sprite de la derecha solo cuesta 1 byte de FLAGS en vez de 16 de RODATA. */
static const uint8_t spr_paddle_l_p0[8] = { 0x7F, 0xFF, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0 };
static const uint8_t spr_paddle_l_p1[8] = { 0x7F, 0xFF, 0xFF, 0x3F, 0xFF, 0xFF, 0x3F, 0xFF };

/* PALA MEDIA (trozo central para la pala expandida): solo relleno. */
#define PADDLE_PAT_MID 3
static const uint8_t spr_paddle_mid_p0[8] = { 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
static const uint8_t spr_paddle_mid_p1[8] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

/* PALA LASER (16x8): cuerpo normal + canon de 2x3 en cada punta.
 * Igual que la pala normal, la punta derecha es la izquierda con FLIP_X. */
#define PADDLE_PAT_LAS_L 13
static const uint8_t spr_paddle_las_l_p0[8] = { 0xFF, 0xFF, 0xFF, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0 };
static const uint8_t spr_paddle_las_l_p1[8] = { 0xC0, 0xC0, 0xC0, 0x3F, 0xFF, 0xFF, 0x3F, 0xFF };

/* RAYO LASER (8x8): barra vertical con nucleo claro (idx3) y halo (idx2). */
#define LASER_PAT 15
static const uint8_t spr_laser_p0[8] = { 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18 };
static const uint8_t spr_laser_p1[8] = { 0x3C, 0x3C, 0x3C, 0x3C, 0x3C, 0x3C, 0x3C, 0x3C };

/* --- Sprite de la BOLA (circulo 6x6 centrado en el tile de 8x8) con BISEL.
 * Ocupa filas 1-6 y columnas 1-6; ball_draw() compensa con BALL_OFF = -1.
 * Bisel tipo esfera (luz desde arriba-izquierda), con 3 tonos de GRIS:
 *   indice 3 = claro  -> gris claro  (arriba-izquierda)
 *   indice 2 = medio  -> blanco/gris (centro)
 *   indice 1 = oscuro -> gris oscuro (abajo-derecha) */
#define BALL_PAT     4
static const uint8_t spr_ball_p0[8] = { 0x00, 0x3C, 0x7A, 0x76, 0x6E, 0x5E, 0x3C, 0x00 };
static const uint8_t spr_ball_p1[8] = { 0x00, 0x3C, 0x7C, 0x78, 0x70, 0x60, 0x00, 0x00 };

/* ---------------------------------------------------------------------------
 * CAPSULAS (power-ups): sprites 8x8 (pastilla con letra). Generados con
 * tools/gen_capsules.py. Indices: 3=claro (marco/letra), 2=medio (relleno).
 * --------------------------------------------------------------------------- */
#define CAPS_PAT_E   5   /* expandir pala        */
#define CAPS_PAT_N   6   /* encoger pala (malo)  */
#define CAPS_PAT_C   7   /* catch                */
#define CAPS_PAT_L   8   /* laser                */
#define CAPS_PAT_D   9   /* disruption (3 bolas) */
#define CAPS_PAT_S  10   /* slow                 */
#define CAPS_PAT_B  11   /* break                */
#define CAPS_PAT_P  12   /* vida extra           */

static const uint8_t caps_E_p0[8] = { 0x00, 0x7E, 0x91, 0x99, 0x91, 0x9D, 0x7E, 0x00 };
static const uint8_t caps_E_p1[8] = { 0x00, 0x7E, 0xFF, 0xFF, 0xFF, 0xFF, 0x7E, 0x00 };
static const uint8_t caps_N_p0[8] = { 0x00, 0x7E, 0x9D, 0x9D, 0x95, 0x95, 0x7E, 0x00 };
static const uint8_t caps_N_p1[8] = { 0x00, 0x7E, 0xFF, 0xFF, 0xFF, 0xFF, 0x7E, 0x00 };
static const uint8_t caps_C_p0[8] = { 0x00, 0x7E, 0x91, 0x91, 0x91, 0x8D, 0x7E, 0x00 };
static const uint8_t caps_C_p1[8] = { 0x00, 0x7E, 0xFF, 0xFF, 0xFF, 0xFF, 0x7E, 0x00 };
static const uint8_t caps_L_p0[8] = { 0x00, 0x7E, 0x91, 0x91, 0x91, 0x9D, 0x7E, 0x00 };
static const uint8_t caps_L_p1[8] = { 0x00, 0x7E, 0xFF, 0xFF, 0xFF, 0xFF, 0x7E, 0x00 };
static const uint8_t caps_D_p0[8] = { 0x00, 0x7E, 0x95, 0x95, 0x95, 0x99, 0x7E, 0x00 };
static const uint8_t caps_D_p1[8] = { 0x00, 0x7E, 0xFF, 0xFF, 0xFF, 0xFF, 0x7E, 0x00 };
static const uint8_t caps_S_p0[8] = { 0x00, 0x7E, 0x91, 0x89, 0x85, 0x99, 0x7E, 0x00 };
static const uint8_t caps_S_p1[8] = { 0x00, 0x7E, 0xFF, 0xFF, 0xFF, 0xFF, 0x7E, 0x00 };
static const uint8_t caps_B_p0[8] = { 0x00, 0x7E, 0x95, 0x99, 0x95, 0x99, 0x7E, 0x00 };
static const uint8_t caps_B_p1[8] = { 0x00, 0x7E, 0xFF, 0xFF, 0xFF, 0xFF, 0x7E, 0x00 };
static const uint8_t caps_P_p0[8] = { 0x00, 0x7E, 0x95, 0x99, 0x91, 0x91, 0x7E, 0x00 };
static const uint8_t caps_P_p1[8] = { 0x00, 0x7E, 0xFF, 0xFF, 0xFF, 0xFF, 0x7E, 0x00 };

/* ===========================================================================
 * SLOTS DE OAM Y PALETAS
 * =========================================================================== */
#define SPR_PADDLE_L  0     /* pala: trozo 1 (siempre el de la izquierda)  */
#define SPR_PADDLE_R  1     /* pala: trozo 2                               */
#define SPR_BALL      2     /* la bola                                     */
#define SPR_PADDLE_3  11    /* pala: trozo 3                               */
#define SPR_PADDLE_4  15    /* pala: trozo 4 (solo en la pala extendida)   */

#define PAL_WALL      VC_BGPAL_0   /* muro / marco / HUD: indice 3 = blanco  */
#define PAL_BRICK_0   VC_BGPAL_0   /* fila 1: ROJO  (idx3 = blanco, comparte con el muro) */
#define PAL_BRICK_1   VC_BGPAL_1   /* fila 2: NARANJA (indices 1/2/3)        */
#define PAL_BRICK_2   VC_BGPAL_2   /* fila 3: AMARILLO (indices 1/2/3)       */
#define PAL_TEXT      VC_BGPAL_0   /* tinta de texto (blanca)                */
#define PAL_TITLE     VC_BGPAL_2   /* tinta del banner                       */

/* --- TEXTOS COMPARTIDOS ---------------------------------------------------
 * cc65 deduplica literales identicos, asi que la MISMA cadena usada en varias
 * pantallas ocupa RODATA una sola vez. Estos defines evitan repetir el literal
 * (y ademas garantizan que las copias sean byte a byte iguales, que es lo que
 * permite al compilador reutilizarlas).                                       */
#define TXT_FIRE      "ESPACIO = LANZAR"   /* titulo y game over */
#define TXT_AGAIN     "PULSA BOTON"        /* titulo y game over */

#define PAL_PADDLE_S  VC_SPPAL_0   /* pala: colores exactos del arte          */
#define PAL_BALL_S    VC_SPPAL_1   /* bola: gris con bisel                    */
#define PAL_CAPS_GOOD VC_SPPAL_2   /* capsulas buenas (verde/cian)            */
#define PAL_CAPS_BAD  VC_SPPAL_3   /* capsulas malas  (rojo)                  */

/* Paleta de SPRITE de la pala: AZUL (escala claro/medio/oscuro). Los patrones
 * de la pala usan 3 = claro (bisel), 2 = relleno, 1 = detalle oscuro. */
static const uint16_t pal_paddle[3] = {
    VC_RGB444(0xA, 0xC, 0xF),   /* 1: azul claro  (detalle)   */
    VC_RGB444(0x4, 0x6, 0xD),   /* 2: azul medio  (relleno)   */
    VC_RGB444(0x0, 0x2, 0x6)    /* 3: azul oscuro (bisel)     */
};

/* Paleta de SPRITE de la BOLA: 3 tonos de GRIS para el bisel tipo esfera
 * (1 = gris oscuro, 2 = gris medio, 3 = blanco highlight). */
static const uint16_t pal_ball[3] = {
    VC_RGB444(0x5, 0x5, 0x5),
    VC_RGB444(0xA, 0xA, 0xA),
    VC_RGB444(0xF, 0xF, 0xF)
};

/* Paletas de SPRITE de las CAPSULAS (1 = detalle, 2 = relleno, 3 = marco/letra). */
static const uint16_t pal_caps_good[3] = {
    VC_RGB444(0x0, 0x4, 0x0),
    VC_RGB444(0x2, 0xE, 0x2),
    VC_RGB444(0xF, 0xF, 0xF)
};
static const uint16_t pal_caps_bad[3] = {
    VC_RGB444(0x4, 0x0, 0x0),
    VC_RGB444(0xE, 0x2, 0x2),
    VC_RGB444(0xF, 0xF, 0xF)
};

/* Recarga las 4 paletas de sprite programables de una vez (bucle compacto).
 * Se llaman desde setup_video: son constantes en RAM, NO en RODATA. */
static void spr_palettes_set(void) {
    uint8_t i;
    for (i = 0; i < 3; i++) {
        vc_pal_set_spr(PAL_PADDLE_S,   (uint8_t)(i + 1), pal_paddle[i]);
        vc_pal_set_spr(PAL_BALL_S,     (uint8_t)(i + 1), pal_ball[i]);
        vc_pal_set_spr(PAL_CAPS_GOOD,  (uint8_t)(i + 1), pal_caps_good[i]);
        vc_pal_set_spr(PAL_CAPS_BAD,   (uint8_t)(i + 1), pal_caps_bad[i]);
    }
}

/* ===========================================================================
 * CONSTANTES DEL JUEGO
 * =========================================================================== */
#define PADDLE_H      8
#define PADDLE_Y      206   /* Y fija de la pala (justo sobre el suelo)       */
#define PADDLE_SPEED  4     /* pixeles por frame con el joystick/tecla        */

/* --- Capsulas (power-ups) --- */
#define CAPS_SPEED    1     /* pixeles por frame de caida (Q0, lento)         */
#define CAPS_SIZE     8     /* lado del sprite de capsula                     */
#define MAX_CAPS      2     /* capsulas simultaneas como maximo               */
/* Separacion vertical MINIMA entre dos capsulas, para que nunca queden a la
 * misma altura (el core solo dibuja 8 sprites por linea horizontal). Como todas
 * caen a la misma velocidad, con 16 px (2 celdas) la separacion se mantiene. */
#define CAPS_MIN_GAP 16
#define CAPS_CHANCE   3     /* 1 de cada N bloques roto suelta capsula        */
#define CAPS_SLOT0    3     /* primer slot de OAM para las capsulas           */

/* Tipos de capsula (efecto). */
#define CAPS_NONE   0
#define CAPS_E      1       /* expandir pala      (buena) */
#define CAPS_N      2       /* encoger pala       (mala)  */
#define CAPS_C      3       /* catch              (buena) */
#define CAPS_L      4       /* laser              (buena) */
#define CAPS_D      5       /* disruption         (buena) */
#define CAPS_S      6       /* slow               (buena) */
#define CAPS_B      7       /* break              (buena) */
#define CAPS_P      8       /* vida extra         (buena) */

/* Duracion de los efectos temporales (en frames, ~60/s). */
#define FX_TIME     600     /* ~10 segundos */

/* Anchuras de pala (en pixeles). Cada trozo = 1 sprite de 8 px.
 *   CORTA    = 2 sprites (16 px)
 *   NORMAL   = 3 sprites (24 px)  <- por defecto
 *   EXPAND   = 4 sprites (32 px)
 * Con la pala NORMAL (16 px) alargo el juego: la bola va mas rapido, y una
 * pala ancha compensa. */
#define PADDLE_W_NORMAL  24
#define PADDLE_W_EXPAND  32
#define PADDLE_W_SHRINK  16

/* Velocidades de la bola en Q8.8 (subpixeles/frame). Suben por nivel.
 * Se guardan como MAGNITUD (>= 0) mas un flag de direccion por eje, para
 * evitar enteros con signo en Q8.8 (cc65 avisa de desbordamiento).
 *
 * OJO al elegir las constantes: los pasos del bucle son ENTEROS, asi que una
 * velocidad fraccionaria se reparte en saltos de N y N+1 pixeles. Si el reparto
 * queda "desigual" (p. ej. 1 1 1 2 1 1 1 2) la bola da TIRONES; si alterna
 * limpio (1 2 1 2) se ve fluida.
 *
 * Con L1=1.50 e INC=+0.50 todos los niveles quedan con reparto PERFECTO y el
 * salto entre niveles es siempre el mismo (+33%), de modo que ningun nivel se
 * siente como un cambio brusco:
 *
 *   nivel   1     2     3     4     5+ (tope)
 *   px/f   1.50  2.00  2.50  3.00  3.50
 *   pasos  1212  2222  2323  3333  3434
 *
 * (Antes era L1=1.00 con INC=+0.25: el nivel 1 iba muy lento y el salto al 2,
 *  de un +25% sobre una base lenta, se notaba muchisimo.) */
#define BALL_SPEED_L1 0x0180   /* 1.50 px/frame por eje (nivel 1)            */
#define BALL_SPEED_INC 0x80    /* +0.50 px/frame por nivel                    */
/* Tope: el recorrido por frame NO debe acercarse al alto de un ladrillo (8 px)
 * ni al diametro de la bola (6), o la fisica se los "saltaria" y la colision
 * fallaria. Con 3.5 px/frame queda margen y sigue siendo exigente. */
#define BALL_SPEED_MAX 0x0380  /* 3.50 px/frame (se alcanza en el nivel 5)    */

#define START_LIVES   3

/* Teclas UART (incluye los codigos que algunos terminales envian para flechas). */
#define KEY_LEFT      'a'
#define KEY_LEFT2     '4'
#define KEY_RIGHT     'd'
#define KEY_RIGHT2    '6'
#define KEY_LAUNCH    ' '
#define KEY_QUIT      'q'

/* ===========================================================================
 * ESTADO
 * =========================================================================== */
static uint8_t  brick[BRICK_ROWS][BRICK_COLS];  /* 0 = vacio, si no = tipo+1   */
static uint8_t  bricks_left;

/* Palas: posicion X entera (pixeles). */
static int16_t  paddle_x;
static int16_t  paddle_vx;      /* -1, 0, +1: direccion pedida */

/* Bolas: MULTI-BOLA. Cada bola lleva posicion Q8.8 (8 bits entero + 8 de
 * fraccion), direccion (dx/dy) y fraccion acumulada. balls[0] es la principal.
 * active = 1 si la bola esta en juego. Se usa uint16_t y flags de signo
 * separados (el Q8.8 con signo no se lleva bien en cc65). */
#define MAX_BALLS  3
#define SPR_BALL0  2      /* slots de OAM de las bolas (2, 12, 13) */
#define SPR_BALL1  12
#define SPR_BALL2  13
typedef struct {
    uint16_t x, y;        /* posicion Q8.8 (0..~0xDF00) */
    uint16_t frac_x;      /* fraccion acumulada del eje X */
    uint16_t frac_y;      /* fraccion acumulada del eje Y */
    int16_t  vx, vy;      /* VELOCIDAD por eje en Q8.8, CON SIGNO:
                           *   el signo dice la direccion y el valor la rapidez.
                           *   Asi el angulo del rebote no esta limitado a 45
                           *   grados: cada eje lleva su propia velocidad.  */
    uint8_t  active;      /* 1 = en juego, 0 = inactiva   */
    uint8_t  stuck;       /* 1 = pegada a la pala (aun sin lanzar) */
} ball_t;
static ball_t balls[MAX_BALLS];
/* Velocidad base del eje Y (magnitud Q8.8, sube con el nivel). Es la que fija el
 * angulo: al rebotar en la pala solo cambia vx (segun la zona), vy se queda.
 * No hay constante equivalente para el eje X: la pone paddle_zone_vx[]. */
static int16_t  ball_vy;
static uint8_t  balls_live;          /* bolas activas (para saber si se pierde vida) */

/* Capsulas (power-ups) que caen. Se guardan como posicion entera (pixel) + tipo. */
typedef struct { uint8_t active, x, y, type; } capsule_t;
static capsule_t caps[MAX_CAPS];

/* Rayos laser (proyectiles de la pala). SUBEN hasta el techo; se disparan los
 * dos a la vez, uno por punta de la pala. */
#define LASER_SLOT0 14    /* rayo de la punta IZQUIERDA                  */
#define LASER_SLOT1 16    /* rayo de la punta DERECHA                    */
#define LASER_COOLDOWN 14 /* ~0.25 s entre disparos                      */
static uint8_t laser_active;   /* 1 = rayo izquierdo en vuelo                 */
static uint8_t laser_x, laser_y;
static uint8_t laser_active2;  /* 1 = rayo derecho en vuelo                   */
static uint8_t laser_x2, laser_y2;
static uint8_t laser_cool;     /* frames de recarga entre disparos            */

/* Declaraciones adelantadas (se definen mas abajo, pero brick_break ya las usa). */
static void caps_spawn(uint8_t col, uint8_t row, uint8_t type);
static void ball_set_speed(void);
static void break_px(uint16_t px, uint16_t py);

static uint8_t ball_any_stuck(void);   /* usada por laser_fire() */

/* --- Efectos activos (power-ups) ---
 * Los efectos duran un tiempo (contador en frames) y luego expiran. */
typedef struct {
    uint8_t  paddle_w;      /* ancho de la pala: 16 corta, 24 normal, 32 expandida */
    uint8_t  slow;          /* 1 = bola lenta activa                                    */
    uint8_t  brk;           /* 1 = la bola atraviesa bloques (break)                     */
    uint8_t  catch;         /* 1 = la pala atrapa la bola (catch)                        */
    uint8_t  laser;         /* 1 = la pala dispara rayos (laser)                         */
    uint16_t t_slow;        /* frames restantes de slow (0 = inactivo)                   */
    uint16_t t_brk;         /* frames restantes de break                                 */
    uint16_t t_catch;       /* frames restantes de catch                                 */
    uint16_t t_paddle;      /* frames restantes de widen/shrink                           */
    uint16_t t_laser;       /* frames restantes de laser                                 */
} effects_t;
static effects_t fx;

static uint16_t score;
static uint8_t  lives;
static uint8_t  level;
static uint8_t  quit_flag;

/* Ultimos valores dibujados en el HUD (para no reescribir VRAM cada frame). */
static uint16_t hud_score_last;
static uint8_t  hud_lives_last;
static uint8_t  hud_level_last;
static uint8_t  game_over;
static uint8_t  level_clear;         /* 1 = nivel superado                     */

static uint16_t rng_state;

/* ===========================================================================
 * ALEATORIO (xorshift de 16 bits)
 * =========================================================================== */
/* NO es 'static': lo usa tambien ball.s (la fisica en ASM) para el rebote
 * aleatorio en el centro de la pala. */
uint8_t rng_next(void) {
    rng_state ^= (uint16_t)(rng_state << 7);
    rng_state ^= (uint16_t)(rng_state >> 9);
    rng_state ^= (uint16_t)(rng_state << 8);
    rng_state = (uint16_t)(rng_state + 0x9E37u);   /* mezcla extra (barato) */
    return (uint8_t)(rng_state & 0xFF);
}

/* Siembra el RNG con fuentes no deterministas (SID $D41B + temporizador real +
 * contador de arranques). Asi el nivel y los rebotes cambian en cada partida. */
static uint8_t rng_boot_count;
static void rng_seed(void) {
    uint32_t t  = rom_get_micros();
    uint8_t  s0 = snd_random();
    uint8_t  s1 = snd_random();

    rng_boot_count++;
    rng_state  = (uint16_t)((uint16_t)s0 << 8) | s1;
    rng_state ^= (uint16_t)t;
    rng_state ^= (uint16_t)(t >> 16) << 8;
    rng_state ^= (uint16_t)rng_boot_count * 0x9E37u;

    if (rng_state == 0) rng_state = 0xA5A5;
    (void)rng_next();
    (void)rng_next();
}

/* ===========================================================================
 * TEXTO (fuente del sistema: tile = ASCII, tinta = color 3 de la paleta)
 * =========================================================================== */
/* Escribe un texto. SOLO con cadenas literal: cc65 lo copia con un bucle
 * indexado inline (no necesita memcpy). Es la version que usa todo el juego. */
static void put_str_pal(uint8_t col, uint8_t row, const char *s, uint8_t pal) {
    uint8_t i = 0;
    while (s[i] && col < SCREEN_COLS) {
        vc_put_cell(col, row, (uint8_t)s[i]);
        vc_set_cell_attr(col, row, pal, 0);
        col++;
        i++;
    }
}

static void put_str_at(uint8_t col, uint8_t row, const char *s) {
    put_str_pal(col, row, s, PAL_TEXT);
}

static void clear_str_at(uint8_t col, uint8_t row, uint8_t len) {
    while (len-- && col < SCREEN_COLS) {
        vc_put_cell(col, row, VC_CHAR_SPACE);
        vc_set_cell_attr(col, row, PAL_TEXT, 0);
        col++;
    }
}

/* Escribe un uint16 decimal alineado a la derecha en (col,row), 'dig' digitos. */
static void put_u16_at(uint8_t col, uint8_t row, uint16_t v, uint8_t dig) {
    uint8_t i;
    for (i = 0; i < dig; i++) {
        uint8_t d = (uint8_t)(v % 10);
        vc_put_cell((uint8_t)(col + dig - 1 - i), row, (uint8_t)('0' + d));
        vc_set_cell_attr((uint8_t)(col + dig - 1 - i), row, PAL_TEXT, 0);
        v = (uint16_t)(v / 10);
    }
}

/* ===========================================================================
 * UTILIDADES DE CELDA
 * =========================================================================== */
static void paint(uint8_t col, uint8_t row, uint8_t tile, uint8_t pal) {
    vc_put_cell(col, row, tile);
    vc_set_cell_attr(col, row, pal, 0);
}

/* Paleta del ladrillo segun su fila. Hay solo 3 paletas de color usables (la 3
 * es BG_COLOR), asi que 5 filas usan un degradado SIMETRICO que repite paletas:
 *   fila 0 = ROJO, fila 1 = NARANJA, fila 2 = AMARILLO, fila 3 = NARANJA,
 *   fila 4 = ROJO.  (rojo -> naranja -> amarillo -> naranja -> rojo)
 * La paleta 0 (ROJO) comparte el indice 3 (blanco) con el muro; los indices 1 y
 * 2 son el rojo medio y oscuro. */
static uint8_t brick_pal(uint8_t row) {
    if (row == 1 || row == 3) return PAL_BRICK_1;   /* naranja */
    if (row == 2)             return PAL_BRICK_2;   /* amarillo */
    return PAL_BRICK_0;                             /* rojo (filas 0 y 4) */
}

/* ===========================================================================
 * TABLERO
 * =========================================================================== */

/* Limpia TODA la pantalla (tilemap + atributos) con los volcados masivos del
 * core (rapidos: 2 operaciones en ensamblador, no un bucle celda a celda).
 * Debe llamarse dentro del VBLANK. */
static void screen_clear(void) {
    vc_fill_tilemap(TILE_BLANK);
    vc_clear_attr();
}

/* Dibuja el marco de muros del campo. El interior ya debe estar limpio
 * (screen_clear()); aqui solo se pintan las celdas del marco (~130). */
static void field_draw(void) {
    uint8_t x, y;

    for (x = FIELD_LEFT; x <= FIELD_RIGHT; x++) {
        paint(x, FIELD_TOP, TILE_WALL, PAL_WALL);
        paint(x, FIELD_BOTTOM, TILE_WALL, PAL_WALL);
    }
    for (y = (uint8_t)(FIELD_TOP + 1); y < FIELD_BOTTOM; y++) {
        paint(FIELD_LEFT, y, TILE_WALL, PAL_WALL);
        paint(FIELD_RIGHT, y, TILE_WALL, PAL_WALL);
    }
}

/* ===========================================================================
 * PALETAS DE LADRILLO POR NIVEL
 * ===========================================================================
 * Las paletas de fondo son GLOBALES al frame, asi que los ladrillos de cada
 * fila usan una de las 3 paletas usables (0/1/2; la 3 es BG_COLOR). Para que
 * cada nivel se vea distinto, REPROGRAMAMOS esas 3 paletas al empezar el nivel
 * con uno de varios esquemas de color.
 *
 * Cada esquema son 9 colores RGB444 en este orden:
 *   [0..2] = paleta 0: idx3 (blanco fijo, muro), idx2 (medio), idx1 (oscuro)
 *   [3..5] = paleta 1: claro, medio, oscuro
 *   [6..8] = paleta 2: claro, medio, oscuro
 * Los valores estan precalculados (antes se derivaban con una funcion, pero el
 * codigo de mul/div no cabia en la RAM).
 * --------------------------------------------------------------------------- */
#define NUM_SCHEMES 3
static const uint16_t scheme_colors[NUM_SCHEMES][9] = {
    /* 0: ROJO / NARANJA / AMARILLO (clasico) */
    { 0xFFF, 0xD22, 0x711,   0xFA2, 0xE72, 0x731,   0xFF4, 0xED2, 0x771 },
    /* 1: ROSA / MORADO / CIAN */
    { 0xFFF, 0xE4A, 0x725,   0xF5E, 0x93D, 0x416,   0x4FF, 0x3DD, 0x167 },
    /* 2: VERDE / CIAN / AZUL */
    { 0xFFF, 0x5D3, 0x261,   0x4FF, 0x3CC, 0x166,   0x47F, 0x35E, 0x127 },
};

/* Programa las 3 paletas de ladrillo segun el nivel (rota entre los esquemas).
 * El orden de las 9 entradas es: paleta 0,1,2 x color 3,2,1. Se recorre con un
 * bucle (mas compacto que 9 llamadas). */
static void brick_palettes_set(uint8_t lv) {
    const uint16_t *c = scheme_colors[(uint8_t)((lv - 1) % NUM_SCHEMES)];
    uint8_t i;
    for (i = 0; i < 9; i++) {
        uint8_t pal = (uint8_t)(i / 3);       /* 0,0,0,1,1,1,2,2,2 */
        uint8_t col = (uint8_t)(3 - (i % 3)); /* 3,2,1,3,2,1,3,2,1 */
        vc_pal_set_bg(pal, col, c[i]);
    }
}

/* Pinta un LADRILLO (2 celdas = mitad izq + mitad der) en el tilemap. */
static void brick_paint(uint8_t br, uint8_t bc) {
    uint8_t x = (uint8_t)(BRICK_X0 + bc * BRICK_W);
    uint8_t y = (uint8_t)(BRICK_Y0 + br);
    uint8_t pal = brick_pal(br);
    paint(x, y, TILE_BRICK_L, pal);
    paint((uint8_t)(x + 1), y, TILE_BRICK_R, pal);
}

/* Borra un LADRILLO (sus 2 celdas) del tilemap. */
static void brick_erase(uint8_t br, uint8_t bc) {
    uint8_t x = (uint8_t)(BRICK_X0 + bc * BRICK_W);
    uint8_t y = (uint8_t)(BRICK_Y0 + br);
    paint(x, y, TILE_BLANK, PAL_TEXT);
    paint((uint8_t)(x + 1), y, TILE_BLANK, PAL_TEXT);
}

/* Coloca toda la rejilla de ladrillos (empieza/reinicia nivel).
 * Las 5 filas son completas (13 ladrillos de 2 celdas = 26 celdas de ancho). */
static void bricks_init(void) {
    uint8_t r, c;

    for (r = 0; r < BRICK_ROWS; r++) {
        uint8_t val = (uint8_t)(r + 1);
        for (c = 0; c < BRICK_COLS; c++) {
            brick[r][c] = val;
            brick_paint(r, c);
        }
    }
    bricks_left = BRICK_COUNT;
}

/* Devuelve el tipo de ladrillo (0 = ninguno) de una celda del campo.
 * La validacion del rango va inline (y no en un helper) porque con -Os el
 * compilador genera un codigo mas compacto repitiendola en cada sitio. */
static uint8_t brick_at(uint8_t col, uint8_t row) {
    if (col < BRICK_X0 || col >= (uint8_t)(BRICK_X0 + BRICK_COLS * BRICK_W)) return 0;
    if (row < BRICK_Y0 || row >= (uint8_t)(BRICK_Y0 + BRICK_ROWS)) return 0;
    return (uint8_t)(brick[row - BRICK_Y0][(uint8_t)((col - BRICK_X0) / BRICK_W)]);
}

/* Golpea el LADRILLO (ambas celdas) que ocupa la celda (col,row).
 * Devuelve: 0 = no habia ladrillo; 1 = roto. */
static uint8_t brick_break(uint8_t col, uint8_t row) {
    uint8_t br, bc, v, hits;
    if (col < BRICK_X0 || col >= (uint8_t)(BRICK_X0 + BRICK_COLS * BRICK_W)) return 0;
    if (row < BRICK_Y0 || row >= (uint8_t)(BRICK_Y0 + BRICK_ROWS)) return 0;
    br = (uint8_t)(row - BRICK_Y0);
    bc = (uint8_t)((col - BRICK_X0) / BRICK_W);
    v  = brick[br][bc];
    if (!v) return 0;

    brick[br][bc] = 0;
    brick_erase(br, bc);
    if (bricks_left) bricks_left--;

    /* Las constantes de descarte se calculan en tiempo de compilacion: asi el
     * '%' no arrastra las rutinas udiv/umod de 16 bits (como ya hace
     * ball_set_speed). Con esto ya salen capsulas rojas (N) y verdes.
     * ~1 de cada CAPS_CHANCE bloques suelta capsula. */
    hits  = (uint8_t)(BRICK_COUNT / CAPS_CHANCE);
    if ((uint8_t)(rng_next() % hits) == 0) {
        uint8_t type = (uint8_t)(1 + (rng_next() % 8));   /* CAPS_E..CAPS_P */
        caps_spawn((uint8_t)(BRICK_X0 + bc * BRICK_W), row, type);
    }
    return 1;
}

/* ===========================================================================
 * PALA Y BOLA (sprites)
 * =========================================================================== */

/* Coloca la pala en su X actual. Cada trozo es un sprite de 8 px, asi que el
 * ancho manda cuantos se usan:
 *   16 px -> [L][R]                (corta: 2 sprites)
 *   24 px -> [L][MID][R]           (normal: 3 sprites)  <- por defecto
 *   32 px -> [L][MID][MID][R]      (expandida: 4 sprites)
 * Los trozos tienen que COMPONER una barra continua: las puntas llevan el bisel
 * de los extremos y todo lo de en medio es relleno (PADDLE_PAT_MID). Por eso la
 * pala corta NO usa el patron SHORT (que tiene bisel en los DOS lados y se ve
 * una costura en el centro) sino las dos puntas [L][R] juntas.
 *
 * La punta DERECHA no tiene patron propio: es la izquierda con VC_SPR_FLIP_X
 * (el core voltea sprites de forma fiable). Asi se ahorran 32 bytes de RODATA. */

/* Dibuja un trozo de pala. flip!=0 -> se espeja en horizontal. */
static void paddle_piece(uint8_t slot, uint16_t x, uint8_t pat, uint8_t flip) {
    vc_sprite_move(slot, x, PADDLE_Y, PAL_PADDLE_S);
    vc_oam_put(slot, VC_OAM_TILE, pat);
    vc_oam_put(slot, VC_OAM_FLAGS, (uint8_t)(PAL_PADDLE_S | (flip ? VC_SPR_FLIP_X : 0)));
}

static void paddle_draw(void) {
    uint8_t  w = fx.paddle_w;
    uint16_t x = (uint16_t)paddle_x;
    /* Patron de las puntas: con laser lleva canon. */
    uint8_t  tip = (uint8_t)(fx.laser ? PADDLE_PAT_LAS_L : PADDLE_PAT_L);

    /* --- Punta IZQUIERDA (siempre) --- */
    paddle_piece(SPR_PADDLE_L, x, tip, 0);

    if (w < PADDLE_W_NORMAL) {
        /* --- CORTA (2 sprites): [L][R] juntos, sin trozo interior --- */
        paddle_piece(SPR_PADDLE_R, (uint16_t)(x + 8), tip, 1);
        vc_sprite_disable(SPR_PADDLE_3);
        vc_sprite_disable(SPR_PADDLE_4);
        return;
    }

    /* --- Trozo INTERIOR (solo si hay 4). Reutiliza el patron de relleno MID. --- */
    if (w > PADDLE_W_NORMAL) {
        paddle_piece(SPR_PADDLE_R, (uint16_t)(x + 8), PADDLE_PAT_MID, 0);
        paddle_piece(SPR_PADDLE_3, (uint16_t)(x + 16), PADDLE_PAT_MID, 0);
        paddle_piece(SPR_PADDLE_4, (uint16_t)(x + 24), tip, 1);
        return;
    }

    /* --- NORMAL (3 sprites): [L][MID][R] --- */
    paddle_piece(SPR_PADDLE_R, (uint16_t)(x + 8), PADDLE_PAT_MID, 0);
    paddle_piece(SPR_PADDLE_3, (uint16_t)(x + 16), tip, 1);
    vc_sprite_disable(SPR_PADDLE_4);
}

/* Dibuja todas las bolas activas. El sprite se desplaza BALL_OFF (-1) para que
 * el circulo de 6x6 quede alineado con el hitbox. */
static uint8_t ball_slot(uint8_t i) {
    return (uint8_t)((i == 0) ? SPR_BALL0 : (i == 1) ? SPR_BALL1 : SPR_BALL2);
}

static void ball_draw(void) {
    uint8_t i;
    for (i = 0; i < MAX_BALLS; i++) {
        uint8_t slot = ball_slot(i);
        if (!balls[i].active) { vc_sprite_disable(slot); continue; }
        /* Redondea la posicion Q8.8 al pixel mas cercano (+0x80) para que el
         * movimiento fraccionario se vea mas fluido (menos "a saltos"). */
        vc_sprite_move(slot,
                       (uint16_t)(((balls[i].x + 0x80) >> 8) + BALL_OFF),
                       (uint8_t)(((balls[i].y + 0x80) >> 8) + BALL_OFF),
                       PAL_BALL_S);
        vc_oam_put(slot, VC_OAM_TILE, BALL_PAT);
    }
}

static void sprites_hide(void) {
    uint8_t i;
    vc_sprite_disable(SPR_PADDLE_L);
    vc_sprite_disable(SPR_PADDLE_R);
    vc_sprite_disable(SPR_PADDLE_3);
    vc_sprite_disable(SPR_PADDLE_4);
    for (i = 0; i < MAX_BALLS; i++) vc_sprite_disable(ball_slot(i));
    for (i = 0; i < MAX_CAPS; i++) vc_sprite_disable((uint8_t)(CAPS_SLOT0 + i));
    vc_sprite_disable(LASER_SLOT0);
    vc_sprite_disable(LASER_SLOT1);
}

/* ===========================================================================
 * CAPSULAS (power-ups)
 * ===========================================================================
 * Una capsula es un sprite que cae recto a velocidad constante. Al tocar la
 * pala, se recoge (de momento solo desaparece; los efectos se anaden despues).
 * --------------------------------------------------------------------------- */

/* Patron de sprite segun el tipo de capsula. Tabla indexada por tipo
 * (CAPS_NONE=0 es una entrada comodin). Mas compacto que un switch. */
static const uint8_t caps_pats[9] = {
    CAPS_PAT_E, CAPS_PAT_E, CAPS_PAT_N, CAPS_PAT_C, CAPS_PAT_L,
    CAPS_PAT_D, CAPS_PAT_S, CAPS_PAT_B, CAPS_PAT_P
};
static uint8_t caps_pattern(uint8_t type) {
    return caps_pats[type];
}

/* Paleta segun tipo: la N (malo) usa la paleta mala; el resto, la buena. */
static uint8_t caps_pal(uint8_t type) {
    return (uint8_t)((type == CAPS_N) ? PAL_CAPS_BAD : PAL_CAPS_GOOD);
}

/* Busca un slot libre para una capsula. Devuelve el indice o 0xFF si no hay. */
static uint8_t caps_free_slot(void) {
    uint8_t i;
    for (i = 0; i < MAX_CAPS; i++) if (!caps[i].active) return i;
    return 0xFF;
}

/* Suelta una capsula en la celda (col,row) con el tipo dado.
 *
 * IMPORTANTE (limite de sprites por linea): el core solo dibuja 8 sprites en
 * una misma linea horizontal. En la linea de la pala ya pueden coincidir la
 * pala (3 sprites), las 3 bolas del multi-bola y el rayo, asi que dos capsulas
 * A LA MISMA ALTURA desbordarian y el core dejaria de dibujar alguna.
 *
 * Como todas las capsulas caen a la MISMA velocidad, basta con no soltar una
 * nueva si ya hay otra a menos de 16 px (2 celdas): a partir de ahi bajan en
 * paralelo y la separacion se mantiene para siempre (nunca se alcanzan).
 * Las posiciones nacen de filas de ladrillo (multiplos de 8), asi que 16 px
 * es justo el minimo que garantiza que no solapen. */
static void caps_spawn(uint8_t col, uint8_t row, uint8_t type) {
    uint8_t i = caps_free_slot();
    uint8_t y = (uint8_t)(row * 8);
    if (i == 0xFF) return;

    /* Con MAX_CAPS = 2 solo hay UN "otro" slot que mirar, asi que no hace
     * falta bucle: se comprueba el que no es 'i'. */
    {
        uint8_t other = (uint8_t)(i ^ 1);      /* 0 <-> 1 */
        uint8_t dy;
        if (caps[other].active) {
            dy = (uint8_t)(caps[other].y > y ? caps[other].y - y : y - caps[other].y);
            if (dy < CAPS_MIN_GAP) return;     /* demasiado cerca: no se suelta */
        }
    }

    caps[i].active = 1;
    caps[i].x = (uint8_t)(col * 8);
    caps[i].y = y;
    caps[i].type = type;
}

/* Dibuja todas las capsulas activas en sus slots de OAM. */
static void caps_draw(void) {
    uint8_t i;
    for (i = 0; i < MAX_CAPS; i++) {
        uint8_t slot = (uint8_t)(CAPS_SLOT0 + i);
        if (!caps[i].active) { vc_sprite_disable(slot); continue; }
        vc_sprite_move(slot, caps[i].x, caps[i].y, caps_pal(caps[i].type));
        vc_oam_put(slot, VC_OAM_TILE, caps_pattern(caps[i].type));
    }
}

/* Avanza las capsulas un frame y comprueba si la pala las recoge.
 * Devuelve el tipo de la ultima capsula recogida (0 = ninguna). */
static uint8_t caps_step(void) {
    uint8_t i, got = CAPS_NONE;
    for (i = 0; i < MAX_CAPS; i++) {
        if (!caps[i].active) continue;
        caps[i].y = (uint8_t)(caps[i].y + CAPS_SPEED);

        /* Fuera por abajo: se pierde. */
        if (caps[i].y > (uint8_t)(PLAY_Y_MAX + 4)) {
            caps[i].active = 0;
            continue;
        }

        /* Toca la pala? (AABB relajado, la capsula es de 8x8).
         * OJO con los cast a uint8_t: paddle_x + fx.paddle_w puede dar 256
         * justo en el borde derecho (232+24), y (uint8_t)256 = 0, con lo que
         * la comparacion fallaria SIEMPRE ahi. Se compara en 16 bits. */
        if ((uint8_t)(caps[i].y + CAPS_SIZE) > PADDLE_Y &&
            caps[i].y < (uint8_t)(PADDLE_Y + PADDLE_H) &&
            (uint16_t)(caps[i].x + CAPS_SIZE) > (uint16_t)paddle_x &&
            (uint16_t)caps[i].x < (uint16_t)(paddle_x + fx.paddle_w)) {
            caps[i].active = 0;
            got = caps[i].type;   /* (el efecto se aplica en play_level) */
        }
    }
    return got;
}

/* ===========================================================================
 * RAYOS LASER (proyectiles de la pala cuando el efecto LASER esta activo)
 * ===========================================================================
 * Se disparan DOS rayos a la vez, uno desde cada punta de la pala.
 *
 * LIMITE DE SPRITES: el core solo dibuja 8 por linea horizontal, y en la linea
 * de la pala pueden coincidir la pala + las 3 bolas del multi-bola + los rayos:
 *     pala normal (3)    + 3 bolas + 2 rayos = 8  -> justo en el limite, OK
 *     pala EXPANDIDA (4) + 3 bolas + 2 rayos = 9  -> OVERFLOW
 * Por eso, con la pala expandida, el rayo DERECHO se omite y solo sale el
 * izquierdo. Es preferible perder un rayo que perder el dibujado de una bola. */

/* Dispara los rayos desde las puntas de la pala. Cada punta tiene su propio
 * rayo, asi que basta con lanzar los que NO esten ya en vuelo. */
static void laser_fire(void) {
    /* Rayo IZQUIERDO (punta izquierda). */
    if (!laser_active) {
        laser_x  = (uint8_t)paddle_x;
        laser_y  = (uint8_t)(PADDLE_Y - 8);
        laser_active = 1;
    }
    /* Rayo DERECHO (punta derecha): se omite con la pala expandida (limite de
     * sprites por linea) y si ya esta en vuelo. */
    if (!laser_active2 && fx.paddle_w <= PADDLE_W_NORMAL) {
        laser_x2 = (uint8_t)(paddle_x + fx.paddle_w - 2);
        laser_y2 = (uint8_t)(PADDLE_Y - 8);
        laser_active2 = 1;
    }
    snd_wall();
}

/* Dibuja los rayos activos (o los apaga si no vuela ninguno). */
static void laser_draw(void) {
    if (laser_active) {
        vc_sprite_move(LASER_SLOT0, laser_x, laser_y, PAL_CAPS_GOOD);
        vc_oam_put(LASER_SLOT0, VC_OAM_TILE, LASER_PAT);
    } else {
        vc_sprite_disable(LASER_SLOT0);
    }
    if (laser_active2) {
        vc_sprite_move(LASER_SLOT1, laser_x2, laser_y2, PAL_CAPS_GOOD);
        vc_oam_put(LASER_SLOT1, VC_OAM_TILE, LASER_PAT);
    } else {
        vc_sprite_disable(LASER_SLOT1);
    }
}

/* Avanza un rayo hacia arriba. Devuelve 1 si sigue vivo, 0 si se perdio.
 * Sube 4 px/frame: con 8 px de ladrillo no se salta ninguna celda. */
static uint8_t laser_advance(uint8_t x, uint8_t *py) {
    uint8_t y = (uint8_t)(*py - 4);
    uint8_t col, row;

    if (y < PLAY_Y_MIN) return 0;                /* llego al techo */
    *py = y;
    col = (uint8_t)((x + 3) >> 3);
    row = (uint8_t)(y >> 3);
    if (brick_at(col, row)) {                    /* impacto en ladrillo */
        break_px((uint16_t)(x + 3), (uint16_t)y);
        return 0;
    }
    return 1;
}

/* Avanza los rayos que esten en vuelo. */
static void laser_step(void) {
    if (laser_active  && !laser_advance(laser_x,  &laser_y))  laser_active  = 0;
    if (laser_active2 && !laser_advance(laser_x2, &laser_y2)) laser_active2 = 0;
}

/* Apaga el rayo (y su recarga). */
static void laser_clear(void) {
    laser_active  = 0;
    laser_active2 = 0;
}

/* Fija la pala centrada y la bola principal pegada encima (una sola bola). */
static void paddle_reset(void) {
    uint8_t i;
    paddle_x  = (int16_t)((PLAY_X_MIN + PLAY_X_MAX - fx.paddle_w) / 2);
    paddle_vx = 0;
    ball_set_speed();   /* velocidad del nivel (y efecto SLOW) ANTES de colocar la bola */

    for (i = 0; i < MAX_BALLS; i++) balls[i].active = 0;
    balls[0].active = 1;
    balls[0].x      = (uint16_t)(((uint16_t)paddle_x + (fx.paddle_w / 2) - (BALL_SIZE / 2)) << 8);
    balls[0].y      = (uint16_t)((uint16_t)(PADDLE_Y - BALL_SIZE) << 8);
    balls[0].frac_x = 0;               /* limpia las fracciones Q8.8 acumuladas */
    balls[0].frac_y = 0;
    balls[0].vx     = ball_vy;              /* sale hacia la derecha... */
    balls[0].vy     = (int16_t)(-ball_vy);  /* ...y hacia arriba (vy negativo) */
    balls[0].stuck  = 1;
    balls_live      = 1;
}

/* Devuelve 1 si alguna bola esta pegada a la pala (stuck). */
static uint8_t ball_any_stuck(void) {
    uint8_t i;
    for (i = 0; i < MAX_BALLS; i++) if (balls[i].active && balls[i].stuck) return 1;
    return 0;
}

/* Suelta todas las bolas pegadas (al pulsar el boton de lanzar). */
static void ball_launch(void) {
    uint8_t i;
    for (i = 0; i < MAX_BALLS; i++) {
        if (balls[i].active) balls[i].stuck = 0;
    }
}

/* DISRUPTION: triplica la bola. Toma la primera bola en juego y crea copias con
 * angulos distintos (izquierda / derecha / recto). Salen disparadas al momento. */
static void ball_split(void) {
    uint8_t i, n = 0;

    ball_set_speed();             /* velocidad del nivel (y efecto SLOW) */

    /* Activa hasta 3 bolas. El barrido de arriba abajo deja a balls[0] de base
     * (siempre activa) y rellena los huecos de abajo: no hace falta buscar cual
     * es la bola "principal". */
    for (i = 0; i < MAX_BALLS; i++) {
        if (balls[i].active) continue;
        if (n >= 2) break;
        balls[i]           = balls[0];      /* copia posicion y fracciones */
        balls[i].active    = 1;
        balls[i].stuck     = 0;             /* las copias salen volando  */
        /* Angulos: una a la izquierda y otra a la derecha, con el MISMO angulo
         * de 45 grados (|vx| = |vy| = ball_vy la primera vez que se recoge). */
        balls[i].vx        = (int16_t)(n == 0 ? -ball_vy : ball_vy);
        balls[i].vy        = (int16_t)(-ball_vy);
        n++;
    }
    /* La base tambien sale volando. */
    balls[0].stuck = 0;

    balls_live = 0;
    for (i = 0; i < MAX_BALLS; i++) if (balls[i].active) balls_live++;
}

/* ===========================================================================
 * MOVIMIENTO DE LA PALA
 * =========================================================================== */
static void paddle_move(void) {
    if (paddle_vx) {
        int16_t nx = (int16_t)(paddle_x + paddle_vx * PADDLE_SPEED);
        /* No sale del interior del campo. */
        if (nx < PLAY_X_MIN) nx = PLAY_X_MIN;
        if (nx > (PLAY_X_MAX - fx.paddle_w + 1)) nx = (int16_t)(PLAY_X_MAX - fx.paddle_w + 1);
        paddle_x = nx;
    }
}

/* ===========================================================================
 * MOVIMIENTO DE LA BOLA + COLISION
 * ===========================================================================
 * Se avanza 1 pixel a la vez. En cada paso, segun la direccion, se prueba la
 * celda de destino (pared o ladrillo) y se rebota el eje afectado.
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * Colision de la bola.
 * ---------------------------------------------------------------------------
 * Estrategia clasica de breakout: en cada paso de 1 pixel se comprueba por
 * separado el eje X y el eje Y contra la celda del centro de la bola.
 *   - Si la celda destino en X es solida -> se rebota en X y se rompe.
 *   - Si la celda destino en Y es solida -> se rebota en Y y se rompe.
 * Asi las esquinas se resuelven solas (rebotan los dos ejes).
 * --------------------------------------------------------------------------- */

/* Celda solida en (px,py): distingue MURO de LADRILLO en una sola pasada
 * (evita llamar otra vez a brick_at, que es lo que costaba codigo).
 * Devuelve: 0 = libre, 1 = muro, 2 = ladrillo. */
static uint8_t solid_kind(uint16_t px, uint16_t py) {
    uint8_t col = (uint8_t)(px >> 3);
    uint8_t row = (uint8_t)(py >> 3);
    if (col == FIELD_LEFT || col == FIELD_RIGHT) return 1;   /* muros laterales */
    if (row == FIELD_TOP) return 1;                          /* techo            */
    return brick_at(col, row) ? 2 : 0;
}

/* Rompe el ladrillo de la celda (en pixeles). */
static void break_px(uint16_t px, uint16_t py) {
    uint8_t r = brick_break((uint8_t)(px >> 3), (uint8_t)(py >> 3));
    if (r) {
        snd_brick();
        score = (uint16_t)(score + 10);
    }
}

/* ===========================================================================
 * ANGULO DE REBOTE EN LA PALA (4 ZONAS)
 * ===========================================================================
 * La pala se divide en 4 tramos y cada uno lanza la bola con un angulo distinto:
 * asi el jugador PUEDE DIRIGIRLA (que es lo que hace el Arkanoid arcade). El
 * signo de vx sale de la tabla (izquierda negativa) y NO depende del azar: la
 * misma zona da siempre el mismo angulo.
 *
 * La velocidad del eje Y la pone ball_vy (sube con el nivel), asi que el angulo
 * se mantiene sea cual sea la velocidad: al rebotar solo cambia vx.
 *
 *   zona:  0 (punta izq)   1 (izq)   2 (der)   3 (punta der)
 *   vx:   -abierto       -suave    +suave    +abierto
 *
 * Valores en Q8.8. Con ball_vy = 0x180 (1.5 px/f del nivel 1) los angulos son:
 *   |vx| 0x0B0 -> 25 grados    |vx| 0x180 -> 45 grados
 * OJO: los valores estan elegidos para que la velocidad total no se dispare
 * (0x180 + 0x180 = 2.1 px/f en el peor caso, dentro del limite seguro).
 * --------------------------------------------------------------------------- */
#define PADDLE_ZONES 4
static const int16_t paddle_zone_vx[PADDLE_ZONES] = {
    -0x180,   /* zona 0: punta IZQUIERDA -> 45 grados a la izquierda */
    -0x0B0,   /* zona 1: izquierda       -> 25 grados a la izquierda */
     0x0B0,   /* zona 2: derecha         -> 25 grados a la derecha   */
     0x180    /* zona 3: punta DERECHA   -> 45 grados a la derecha   */
};

/* NOTA: en el centro (zonas 1 y 2) la bola sale mas recta (25 grados) que en las
 * puntas (45), asi que se ve algo mas lenta: es la rapidez TOTAL la que baja
 * (1.65 px/f frente a 2.12), no el eje Y, que es igual en las 4 zonas. Se probo a
 * igualar la rapidez subiendo tambien el eje Y del centro, pero el codigo de ese
 * caso no cabia en RAM y ademas dejaba el nivel 5 por encima de BALL_SPEED_MAX.
 * Se queda tal cual: las 4 zonas funcionan y el angulo de cada una es correcto. */

/* Zona de golpe (0..3) para una distancia 'rel' desde la punta izquierda.
 * En vez de dividir por el ancho (el '/' arrastraria las rutinas udiv/umod),
 * se busca el tramo con una tabla: los valores de corte son (ancho/4)*1,2,3.
 * ASCII ordenado, con los 2 anchos posibles (corta y normal dan lo mismo). */
static const uint8_t zone_cut[PADDLE_ZONES - 1] = { 6, 12, 18 };
static uint8_t paddle_zone(uint8_t rel) {
    uint8_t z = 0;
    while (z < PADDLE_ZONES - 1 && rel >= zone_cut[z]) z++;
    return z;
}

/* Avanza la bola 'b' un frame completo (pasos de 1 pixel). Devuelve 1 si la bola ha
 * caido por debajo del campo (vida perdida para esa bola).
 *
 * FISICA DE VELOCIDAD POR EJE: la velocidad va en p->vx y p->vy (Q8.8 CON SIGNO).
 * Cada eje lleva su propia rapidez, asi que el angulo NO esta limitado a 45
 * grados: el rebote en la pala puede lanzar la bola a 23, 45 o 66 grados porque
 * reparte la velocidad entre los dos ejes. El signo indica la direccion.
 *
 * IMPORTANTE (algoritmo): se comprueba el BORDE delantero de la bola (no solo
 * su centro), porque un sprite de 8x8 puede solapar dos celdas a la vez. Si se
 * mirase solo el centro, la bola se meteria en el muro antes de rebotar. Para
 * el eje vertical se comprueban los DOS bordes horizontales (izq y der). */
static uint8_t ball_step(uint8_t b) {
    ball_t *p = &balls[b];
    uint8_t  i;
    /* Pasos de cada eje = parte entera + lo que arrastre la fraccion. */
    uint8_t  steps_x, steps_y;

    /* --- Eje X --- */
    p->frac_x = (uint16_t)(p->frac_x + (uint16_t)(p->vx & 0xFF));
    /* OJO: los pasos salen de la MAGNITUD, no de (uint16_t)vx. Si vx es negativo,
     * (uint16_t)vx tiene el bit de signo puesto y su byte alto da 0xFF -> 254
     * pasos en un frame: la bola atravesaria el campo entero de una vez. */
    steps_x = (uint8_t)(((uint16_t)((p->vx < 0) ? -p->vx : p->vx)) >> 8);
    if (p->frac_x >= 0x100) {
        steps_x = (uint8_t)(steps_x + 1);
        p->frac_x = (uint16_t)(p->frac_x - 0x100);
    }
    /* --- Eje Y --- */
    p->frac_y = (uint16_t)(p->frac_y + (uint16_t)(p->vy & 0xFF));
    steps_y = (uint8_t)(((uint16_t)((p->vy < 0) ? -p->vy : p->vy)) >> 8);
    if (p->frac_y >= 0x100) {
        steps_y = (uint8_t)(steps_y + 1);
        p->frac_y = (uint16_t)(p->frac_y - 0x100);
    }

    /* El bucle corre tantas veces como el eje MAS rapido necesite; el otro solo
     * avanza mientras le queden pasos (asi se desacopla la velocidad por eje). */
    {
        uint8_t total = (steps_x > steps_y) ? steps_x : steps_y;
        uint8_t sx = steps_x, sy = steps_y;
        if (total == 0) total = 1;

        for (i = 0; i < total; i++) {
            uint16_t x = (uint16_t)(p->x >> 8);   /* esquina sup-izq (px) */
            uint16_t y = (uint16_t)(p->y >> 8);
            /* Al rebotar en la pala se SALE del bucle ('done'): el impacto ya
             * consume el frame. Sin esto, el paso del eje X que sigue probaria
             * otra vez la pala (la bola esta apoyada en ella) y volveria a
             * rebotar: la bola se "bailaba" encima de la pala sin salir. */
            uint8_t  done = 0;
            /* Borde central (Y) de la bola. Las esquinas delanteras se calculan
             * JUSTO ANTES de cada test, porque el signo de vx/vy puede haber
             * cambiado en el test anterior (y con el, hacia donde esta "delante"). */
            int16_t  cx = (int16_t)(x + BALL_SIZE / 2);  /* centro (x)      */
            int16_t  cy = (int16_t)(y + BALL_SIZE / 2);  /* centro (y)      */
            int16_t  newx, newy, lx, rx, ty, byy;

            /* ============================================================
             * Eje X: solo se prueba si este eje tiene paso pendiente.
             * solid_kind distingue muro (1) de ladrillo (2) en una pasada.
             * ============================================================ */
            if (sx) {
                /* El signo se lee AQUI (no antes), para que un rebote en X del
                 * test anterior no deje el borde delantero mirando al lado
                 * contrario. */
                newx = (int16_t)((p->vx >= 0) ? x + 1 : x - 1);
                lx = newx; rx = (int16_t)(newx + BALL_SIZE - 1);
                {
                int16_t edge_x = (int16_t)((p->vx >= 0) ? rx : lx);
                uint8_t kx = solid_kind((uint16_t)edge_x, (uint16_t)cy);
                if (kx) {
                    if (kx == 2) break_px((uint16_t)edge_x, (uint16_t)cy);
                    else         snd_wall();
                    if (kx == 1 || !fx.brk) {   /* muro siempre rebota; el
                                                 * ladrillo solo sin BREAK */
                        p->vx = (int16_t)(-p->vx);
                        newx = (int16_t)x;
                        lx = newx; rx = (int16_t)(newx + BALL_SIZE - 1);
                    }
                }
                }
            }

            /* ============================================================
             * Eje Y (borde delantero, en AMBOS bordes horizontales)
             * ============================================================ */
            if (sy) {
                newy = (int16_t)((p->vy >= 0) ? y + 1 : y - 1);
                ty = newy; byy = (int16_t)(newy + BALL_SIZE - 1);
                {
                int16_t edge_y = (int16_t)((p->vy >= 0) ? byy : ty);
                uint8_t kL = solid_kind((uint16_t)lx, (uint16_t)edge_y);
                uint8_t kR = kL ? 0 : solid_kind((uint16_t)rx, (uint16_t)edge_y);
                uint8_t ky = kL ? kL : kR;
                if (ky) {
                    uint16_t hx = (uint16_t)(kL ? lx : rx);
                    if (ky == 2) break_px(hx, (uint16_t)edge_y);
                    else         snd_wall();
                    if (ky == 1 || !fx.brk) {
                        p->vy = (int16_t)(-p->vy);
                        newy = (int16_t)y;
                        ty = newy; byy = (int16_t)(newy + BALL_SIZE - 1);
                    }
                }
                }
            }

            /* ============================================================
             * Pala (solo si la bola baja). Solape AABB estricto.
             *
             * OJO: el rebote NO consume el paso del eje Y. Si se dejase 'sy'
             * pendiente, el siguiente paso volveria a probar la pala (la bola
             * sigue solapandola) y rebotaria varias veces seguidas hasta salir
             * por el lado: la bola "nunca es capturada por la pala". Por eso se
             * cancela el paso (sy = 0) y se sale del bucle.
             * ============================================================ */
            if (newy >= 0 && p->vy > 0) {
                int16_t pl = (int16_t)(newy + BALL_SIZE - 1);   /* borde de abajo */
                if ((uint16_t)(pl + 1) > PADDLE_Y && (uint16_t)newy < (PADDLE_Y + PADDLE_H) &&
                    (uint16_t)(rx + 1) > (uint16_t)paddle_x &&
                    (uint16_t)lx < (uint16_t)(paddle_x + fx.paddle_w)) {
                    if (fx.catch) {
                        /* CATCH: la pala atrapa la bola (se queda pegada hasta
                         * volver a lanzarla). */
                        p->stuck = 1;
                        p->vy = 0;               /* al lanzar, saldra hacia arriba */
                        p->y = (uint16_t)((uint16_t)(PADDLE_Y - BALL_SIZE) << 8);
                        snd_wall();
                        return 0;
                    }
                    /* Zona de golpe: 0..3 de izquierda a derecha. El reves X de la
                     * pala no afecta (fx.paddle_w es el ancho real). */
                    {
                        int16_t rel = (int16_t)((lx + BALL_SIZE / 2) - (int16_t)paddle_x);
                        uint8_t zone;
                        if (rel < 0) rel = 0;
                        zone = paddle_zone((uint8_t)rel);
                        /* Velocidad por eje segun la zona (el signo de vx sale
                         * de la tabla: izquierda negativa, derecha positiva).
                         * En el CENTRO (zonas 1 y 2) el eje Y tambien sube, para
                         * que la rapidez total sea la misma que en las puntas y
                         * no se vea "lento" (el angulo no cambia). */
                        p->vx = paddle_zone_vx[zone];
                        p->vy = (int16_t)(-ball_vy);   /* hacia arriba (negativo) */
                    }
                    /* La bola queda APOYADA ENCIMA de la pala (su borde inferior
                     * roza PADDLE_Y), sin solaparla: si se dejase a la altura de
                     * PADDLE_Y se veria metida 6 px dentro de la pala.
                     * El 'done' de abajo ya evita que vuelva a rebotar en el mismo
                     * frame, asi que no hace falta colarla dentro. */
                    p->y = (uint16_t)((uint16_t)(PADDLE_Y - BALL_SIZE) << 8);
                    snd_wall();
                    done = 1;        /* el impacto consume el frame: fuera del bucle */
                    break;
                }
            }

            /* --- Aplica el movimiento de los ejes que tengan paso --- */
            if (sx) {
                p->x = (uint16_t)((p->vx >= 0) ? (p->x + 0x100) : (p->x - 0x100));
                sx--;
            }
            if (sy) {
                p->y = (uint16_t)((p->vy >= 0) ? (p->y + 0x100) : (p->y - 0x100));
                sy--;
            }

            /* --- Red de seguridad: la bola NUNCA puede salir del campo por los
             * lados ni por arriba, pase lo que pase en la deteccion de colision. */
            if ((p->x >> 8) < PLAY_X_MIN) {
                p->x = (uint16_t)((uint16_t)PLAY_X_MIN << 8);
                p->vx = (int16_t)((p->vx < 0) ? -p->vx : p->vx);
            } else if ((p->x >> 8) > BALL_X_MAX) {
                p->x = (uint16_t)((uint16_t)BALL_X_MAX << 8);
                p->vx = (int16_t)((p->vx > 0) ? -p->vx : p->vx);
            }
            if ((p->y >> 8) < PLAY_Y_MIN) {
                p->y = (uint16_t)((uint16_t)PLAY_Y_MIN << 8);
                p->vy = (int16_t)((p->vy < 0) ? -p->vy : p->vy);
            }

            /* --- Fuera por abajo: esta bola se pierde --- */
            if ((p->y >> 8) > PLAY_Y_MAX) return 1;

            /* Rebote en la pala: el impacto ya gasto el frame. */
            if (done) break;
        }
    }
    return 0;
}

/* ===========================================================================
 * VELOCIDAD DE LA BOLA SEGUN EL NIVEL
 * ===========================================================================
 * Magnitud Q8.8 del eje Y; sube con el nivel. El eje X lo fija la zona de la
 * pala (paddle_zone_vx), asi que aqui no hay constante equivalente.
 * =========================================================================== */
static void ball_set_speed(void) {
    int16_t base;
    uint16_t extra = (uint16_t)((level > 1 ? (level - 1) : 0) * BALL_SPEED_INC);
    base = (int16_t)(BALL_SPEED_L1 + extra);
    if (base > (int16_t)BALL_SPEED_MAX) base = (int16_t)BALL_SPEED_MAX;
    if (fx.slow) base = (int16_t)(base / 2);         /* efecto SLOW */
    /* Velocidad del eje Y: fija el angulo del rebote. El eje X lo pone la zona
     * de la pala (paddle_zone_vx), asi que no guarda ninguna constante. */
    ball_vy = base;
}

/* ===========================================================================
 * EFECTOS (power-ups)
 * ===========================================================================
 * Se activan al recoger una capsula. Los temporales se cuentan en frames y al
 * expirar devuelven el estado a la normalidad. */

/* Estado inicial/limpio de los efectos. */
static void fx_reset(void) {
    fx.paddle_w = PADDLE_W_NORMAL;
    fx.slow = 0; fx.brk = 0; fx.catch = 0; fx.laser = 0;
    fx.t_slow = 0; fx.t_brk = 0; fx.t_catch = 0;
    fx.t_paddle = 0; fx.t_laser = 0;
    laser_clear();
}

/* Reajusta paddle_x para que la pala quepa dentro del campo (tras cambiar de ancho). */
static void paddle_clamp(void) {
    if (paddle_x < PLAY_X_MIN) paddle_x = PLAY_X_MIN;
    if (paddle_x > (int16_t)(PLAY_X_MAX - fx.paddle_w + 1))
        paddle_x = (int16_t)(PLAY_X_MAX - fx.paddle_w + 1);
}

/* Aplica el efecto del tipo de capsula recogida. */
static void fx_apply(uint8_t type) {
    switch (type) {
        case CAPS_E:   /* expandir pala */
            fx.paddle_w = PADDLE_W_EXPAND;
            fx.t_paddle = FX_TIME;
            paddle_clamp();
            break;
        case CAPS_N:   /* encoger pala (malo) */
            fx.paddle_w = PADDLE_W_SHRINK;
            fx.t_paddle = FX_TIME;
            paddle_clamp();
            break;
        case CAPS_S:   /* bola lenta */
            fx.slow = 1; fx.t_slow = FX_TIME;
            ball_set_speed();
            break;
        case CAPS_B:   /* bola atraviesa bloques */
            fx.brk = 1; fx.t_brk = FX_TIME;
            break;
        case CAPS_C:   /* catch (la pala atrapa la bola) */
            fx.catch = 1; fx.t_catch = FX_TIME;
            break;
        case CAPS_P:   /* vida extra */
            if (lives < 9) lives++;
            break;
        case CAPS_D:   /* disruption: triplica la bola */
            ball_split();
            break;
        case CAPS_L:   /* laser: la pala dispara rayos */
            fx.laser = 1; fx.t_laser = FX_TIME;
            break;
        default:
            break;
    }
}

/* Tick por frame: decrementa los temporizadores y al expirar desactiva el efecto. */
static void fx_update(void) {
    if (fx.t_slow   && --fx.t_slow   == 0) { fx.slow = 0; ball_set_speed(); }
    if (fx.t_brk    && --fx.t_brk    == 0) { fx.brk = 0; }
    if (fx.t_catch  && --fx.t_catch  == 0) { fx.catch = 0; }
    if (fx.t_paddle && --fx.t_paddle == 0) {
        /* Al expirar, la pala vuelve a su ancho normal (y se reajusta). */
        fx.paddle_w = PADDLE_W_NORMAL;
        paddle_clamp();
    }
    if (fx.t_laser && --fx.t_laser == 0) { fx.laser = 0; laser_clear(); }
}

/* ===========================================================================
 * HUD
 * ===========================================================================
 * En la fila 1 (no la 0: la fila 0 se ve desplazada por el pipeline del core).
 * =========================================================================== */
#define HUD_ROW  1

static void hud_update(void);   /* definida abajo */

static void hud_draw(void) {
    put_str_at(0, HUD_ROW, "PTS");
    put_str_at(14, HUD_ROW, "NIV");
    put_str_at(30, HUD_ROW, "VIDAS");
    /* Fuerza a hud_update() a redibujar los 3 numeros (valores "last" imposibles). */
    hud_score_last = 0xFFFF;
    hud_level_last = 0xFF;
    hud_lives_last = 0xFF;
    hud_update();
}

/* Actualiza solo lo que cambie (evita escribir VRAM cada frame). */
static void hud_update(void) {
    if (score != hud_score_last) {
        put_u16_at(4, HUD_ROW, score, 5);
        hud_score_last = score;
    }
    if (level != hud_level_last) {
        put_u16_at(18, HUD_ROW, level, 2);
        hud_level_last = level;
    }
    if (lives != hud_lives_last) {
        put_u16_at(36, HUD_ROW, lives, 2);
        hud_lives_last = lives;
    }
}

/* ===========================================================================
 * SETUP DE VIDEO
 * =========================================================================== */
static void setup_video(void) {

    vc_wait_ready();
    vc_wait_vblank();
    vc_clear_vram();

    /* Fondo negro: BG_COLOR es la entrada 15; el tile transparente y los huecos
     * de la fuente se ven sobre negro. */
    vc_set_bgcolor(VC_RGB444(0x0, 0x0, 0x0));

    /* Paletas de fondo: el muro/HUD usan la paleta 0 (idx3 = blanco) y los
     * ladrillos las paletas 0/1/2 segun la fila. brick_palettes_set() programa
     * los 3 tonos segun el nivel (cambian de color cada nivel). */
    brick_palettes_set(1);

    vc_set_bgcolor(VC_RGB444(0x0, 0x0, 0x0));

    /* Paletas de sprite (pala azul, bola gris, capsulas) desde constantes. */
    spr_palettes_set();

    /* Patrones de fondo (muro + las 2 mitades del ladrillo). */
    vc_load_bg_pattern(TILE_WALL,    tile_wall_p0,     tile_wall_p0);
    vc_load_bg_pattern(TILE_BRICK_L, tile_brick_l_p0,  tile_brick_l_p1);
    vc_load_bg_pattern(TILE_BRICK_R, tile_brick_r_p0,  tile_brick_r_p1);

    /* Patrones de sprite. La PALA usa sus propios patrones (normal + variantes). */
    vc_load_spr_pattern(PADDLE_PAT_L,     spr_paddle_l_p0,     spr_paddle_l_p1);
    vc_load_spr_pattern(PADDLE_PAT_MID,   spr_paddle_mid_p0,   spr_paddle_mid_p1);
    vc_load_spr_pattern(PADDLE_PAT_LAS_L, spr_paddle_las_l_p0, spr_paddle_las_l_p1);
    vc_load_spr_pattern(LASER_PAT,        spr_laser_p0,        spr_laser_p1);
    vc_load_spr_pattern(BALL_PAT,     spr_ball_p0,     spr_ball_p1);
    /* Capsulas (power-ups). */
    vc_load_spr_pattern(CAPS_PAT_E, caps_E_p0, caps_E_p1);
    vc_load_spr_pattern(CAPS_PAT_N, caps_N_p0, caps_N_p1);
    vc_load_spr_pattern(CAPS_PAT_C, caps_C_p0, caps_C_p1);
    vc_load_spr_pattern(CAPS_PAT_L, caps_L_p0, caps_L_p1);
    vc_load_spr_pattern(CAPS_PAT_D, caps_D_p0, caps_D_p1);
    vc_load_spr_pattern(CAPS_PAT_S, caps_S_p0, caps_S_p1);
    vc_load_spr_pattern(CAPS_PAT_B, caps_B_p0, caps_B_p1);
    vc_load_spr_pattern(CAPS_PAT_P, caps_P_p0, caps_P_p1);

    /* Sin bandas de scroll. */
    vc_set_raster(0xFF, 0xFF);
    vc_set_band2_scroll(0, 0);
    vc_set_band3_scroll(0, 0);

    /* Limpia toda la pantalla (HUD incluido). */
    screen_clear();
}

/* ===========================================================================
 * ENTRADA
 * =========================================================================== */
static void read_input(void) {
    uint8_t j = joy_read();
    uint8_t fire_pressed = 0;
    char c;

    paddle_vx = 0;
    if (j & JOY_A_LEFT)  paddle_vx = -1;
    if (j & JOY_A_RIGHT) paddle_vx = +1;

    while (rom_uart_rx_ready()) {
        c = rom_uart_getc();
        if (c == KEY_LEFT || c == KEY_LEFT2)  paddle_vx = -1;
        else if (c == KEY_RIGHT || c == KEY_RIGHT2) paddle_vx = +1;
        else if (c == KEY_LAUNCH || c == '\r' || c == '\n') {
            fire_pressed = 1;
        }
        else if (c == KEY_QUIT || c == 'Q') quit_flag = 1;
    }

    /* El boton de fuego tambien dispara. */
    if (j & JOY_A_FIRE) fire_pressed = 1;

    /* Si hay bolas pegadas, el boton las lanza; si no y hay laser (y el rayo
     * anterior ya se recargo), dispara. */
    if (fire_pressed) {
        if (ball_any_stuck()) ball_launch();
        else if (fx.laser && laser_cool == 0 && !laser_active) {
            laser_fire();
            laser_cool = LASER_COOLDOWN;
        }
    }
}

/* ===========================================================================
 * PARTIDA
 * =========================================================================== */
static uint8_t play_level(void) {
    uint8_t lost;
    uint8_t i;

    game_over   = 0;
    level_clear = 0;

    vc_wait_vblank();
    brick_palettes_set(level);   /* cambia los colores de los ladrillos por nivel */
    bricks_init();
    fx_reset();
    paddle_reset();
    /* Ninguna capsula en pantalla al empezar el nivel. */
    for (i = 0; i < MAX_CAPS; i++) caps[i].active = 0;
    hud_update();
    vc_wait_vblank_end();

    while (!quit_flag && !game_over && !level_clear) {
        vc_wait_vblank();

        read_input();
        if (quit_flag) { vc_wait_vblank_end(); break; }

        paddle_move();

        /* Recorre TODAS las bolas activas. Las que estan pegadas (stuck) se
         * mueven con la pala; las demas avanzan con la fisica. */
        lost = 0;
        for (i = 0; i < MAX_BALLS; i++) {
            if (!balls[i].active) continue;
            if (balls[i].stuck) {
                balls[i].x = (uint16_t)(((uint16_t)paddle_x + (fx.paddle_w / 2) - (BALL_SIZE / 2)) << 8);
                balls[i].y = (uint16_t)((uint16_t)(PADDLE_Y - BALL_SIZE) << 8);
            } else if (ball_step(i)) {
                /* Esta bola cayo: se desactiva. */
                balls[i].active = 0;
                if (balls_live) balls_live--;
                lost = 1;
            }
        }
        ball_set_speed();   /* velocidad segun nivel/efectos (una vez por frame) */

        /* Si ya no queda ninguna bola en juego: vida perdida. */
        if (lost && balls_live == 0) {
            snd_die();
            lives--;
            if (lives == 0) {
                game_over = 1;
            } else {
                /* Recoloca pala y bola para relanzar; se pierden las capsulas. */
                for (i = 0; i < MAX_CAPS; i++) caps[i].active = 0;
                paddle_reset();
            }
        }

        /* Capsulas: caen; si la pala recoge una, se aplica su efecto. */
        if (!game_over) {
            uint8_t got = caps_step();
            if (got != CAPS_NONE) { snd_bonus(); fx_apply(got); }
        }
        fx_update();

        /* Rayo laser: avanza y rompe ladrillos. Recarga entre disparos. */
        if (laser_cool) laser_cool--;
        if (fx.laser) laser_step();

        /* Redibuja los sprites CADA frame (dentro del VBLANK) para que no se
         * pierdan ni salgan partidos. */
        paddle_draw();
        ball_draw();
        caps_draw();
        laser_draw();

        /* El HUD tambien se toca dentro del VBLANK (solo escribe lo que cambia). */
        hud_update();

        snd_update();
        vc_wait_vblank_end();

        if (bricks_left == 0) level_clear = 1;
    }

    return level_clear;
}

/* ===========================================================================
 * PANTALLA DE BIENVENIDA
 * =========================================================================== */
/* 1 = empezar, 0 = salir */
static uint8_t start_pressed(void) {
    char c;
    if (joy_read() & JOY_A_FIRE) return 1;
    while (rom_uart_rx_ready()) {
        c = rom_uart_getc();
        if (c == ' ' || c == '\r' || c == '\n' || c == 'e' || c == 'E') return 1;
        if (c == 'q' || c == 'Q') return 2;
    }
    return 0;
}

static uint8_t title_screen(void) {
    vc_wait_vblank();
    sprites_hide();          /* por si acaso: arranca sin sprites visibles */
    screen_clear();

    put_str_pal(14, 6, "A R K A N O I D", PAL_TITLE);

    /* Ayuda: se reutilizan las MISMAS cadenas en todas las pantallas (cc65
     * deduplica literales identicos, asi que cada texto distinto ocupa una vez). */
    put_str_pal(10, 16, "A/D = MOVER", PAL_TEXT);
    put_str_pal(10, 18, TXT_FIRE, PAL_TEXT);

    /* Pala de adorno (sprites). La bola NO se dibuja en el titulo: en una
     * pantalla sin juego se veia raro y solo gastaba codigo. */
    fx_reset();
    paddle_x = (int16_t)((VC_SCREEN_W / 2) - (fx.paddle_w / 2));
    paddle_draw();

    vc_wait_vblank_end();

    {
        uint8_t  shown = 0xFF;
        uint16_t hold  = 0;

        for (;;) {
            uint8_t r;
            uint8_t on = (uint8_t)((hold / 40) & 1);

            vc_wait_vblank();
            if (on != shown) {
                if (on) clear_str_at(15, 21, 9);
                else    put_str_pal(15, 21, TXT_AGAIN, PAL_TITLE);
                shown = on;
            }
            /* Recoloca la pala de adorno cada frame. */
            paddle_draw();
            hold++;
            vc_wait_vblank_end();

            r = start_pressed();
            if (r == 1) { sprites_hide(); return 1; }
            if (r == 2) { sprites_hide(); return 0; }
        }
    }
}

/* ===========================================================================
 * DESPEDIDA
 * =========================================================================== */
static void bye_screen(void) {
    uint8_t i;

    vc_wait_vblank();
    sprites_hide();          /* apaga pala y bola antes de limpiar */
    screen_clear();

    put_str_pal(14, 12, "GRACIAS POR", PAL_TEXT);
    put_str_pal(17, 14, "JUGAR!", PAL_TITLE);
    vc_wait_vblank_end();

    for (i = 0; i < 80; i++) {
        vc_wait_vblank();
        vc_wait_vblank_end();
    }
}

/* ===========================================================================
 * MAIN
 * =========================================================================== */
int main(void) {
    uint8_t again;

    rom_uart_puts("\r\nARKANOID - Core de Video\r\n");
    rom_uart_puts("A/D o joystick = mover, ESPACIO = lanzar, q = salir.\r\n");

    setup_video();
    snd_init();
    joy_init();
    fx_reset();          /* estado inicial de los efectos (ancho de pala, etc.) */

    if (!title_screen()) {
        bye_screen();
        snd_silence();
        vc_wait_vblank();
        vc_clear_oam();
        vc_set_scroll_x(0);
        vc_set_scroll_y(0);
        rom_uart_puts("\r\nSaliendo al monitor...\r\n");
        return 0;
    }

    do {
        again = 0;
        rng_seed();

        score = 0;
        level = 1;
        lives = START_LIVES;
        quit_flag = 0;
        vc_wait_vblank();
        screen_clear();
        field_draw();
        hud_draw();
        vc_wait_vblank_end();

        while (!quit_flag) {
            if (play_level()) {
                /* Nivel superado: sube de nivel y acelera. */
                if (level < 9) level++;
                snd_bonus();
                /* Pausa breve con el mensaje (sin tocar el HUD de la fila 0). */
                vc_wait_vblank();
                sprites_hide();      /* oculta pala/bola durante el cartel */
                put_str_pal(13, 15, "NIVEL SUPERADO", PAL_TITLE);
                vc_wait_vblank_end();
                {
                    uint8_t d;
                    for (d = 0; d < 60 && !quit_flag; d++) {
                        vc_wait_vblank();
                        read_input();
                        vc_wait_vblank_end();
                    }
                }
                vc_wait_vblank();
                clear_str_at(13, 15, 14);   /* borra solo "NIVEL SUPERADO" (cols 13..26),
                                             * sin tocar el muro derecho (col 32) */
                vc_wait_vblank_end();
            } else {
                break;   /* game over o salida */
            }
        }

        if (quit_flag) {
            again = 0;
        } else {
            /* GAME OVER */
            snd_silence();
            vc_wait_vblank();
            sprites_hide();          /* apaga pala y bola (no deben flotar) */
            screen_clear();
            put_str_pal(15, 8, "GAME OVER", PAL_TITLE);
            put_str_at(14, 11, "PUNTOS:");
            put_u16_at(21, 11, score, 5);
            /* Ayuda: se reutilizan las MISMAS cadenas del titulo (ahorra RODATA). */
            put_str_at(10, 16, TXT_AGAIN);
            put_str_at(10, 18, TXT_FIRE);
            vc_wait_vblank_end();
            rom_uart_puts("\r\nGAME OVER - BOTON/R = jugar de nuevo, Q = salir\r\n");

            {
                uint8_t d;
                for (d = 0; d < 20; d++) { vc_wait_vblank(); vc_wait_vblank_end(); }
            }
            for (;;) {
                char c;
                if (joy_read() & JOY_A_FIRE) { again = 1; break; }
                if (!rom_uart_rx_ready()) { vc_wait_vblank(); vc_wait_vblank_end(); continue; }
                c = rom_uart_getc();
                if (c == 'r' || c == 'R') { again = 1; break; }
                if (c == 'q' || c == 'Q') { again = 0; break; }
            }
            if (again) rom_uart_puts("\r\nNueva partida.\r\n");
        }
    } while (again);

    bye_screen();
    snd_silence();
    vc_wait_vblank();
    vc_clear_oam();
    vc_set_scroll_x(0);
    vc_set_scroll_y(0);
    rom_uart_puts("\r\nSaliendo al monitor...\r\n");
    return 0;
}
