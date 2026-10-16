#!/usr/bin/env python3
"""
piskel2planes.py - Convierte arte Piskel (colores ARGB) a planos del Core de Video.

El Core de Video dibuja cada tile con DOS planos de bits (plano0, plano1) donde
el indice de color de cada pixel es:   idx = (plano1_bit << 1) | plano0_bit
(idx 0..3; 0 = transparente). El COLOR real lo pone la paleta de la celda.

Este script toma una rejilla de colores hex (formato Piskel 0xAARRGGBB) y un
mapa color->indice, y emite los dos arrays uint8_t[8] listos para pegar en game.c.

Uso:
    python tools/piskel2planes.py
(edita GRID y COLOR_TO_IDX abajo, o importa la funcion planes()).
"""

# Mapa color(Piskel ARGB) -> indice de paleta (1, 2 o 3). El transparente es 0.
COLOR_TO_IDX = {
    0xFFF8A7A6: 3,   # claro  (bisel superior / highlight)
    0xFFEF6E6D: 2,   # medio  (relleno)
    0xFFB10A09: 1,   # oscuro (bisel inferior / sombra)
    0x00000000: 0,   # transparente
}


def planes(grid, c2i=None):
    """grid: lista de 8 filas x 8 columnas de colores hex ARGB (o None/'...' para
    transparente). Devuelve (p0, p1), cada uno lista de 8 bytes."""
    if c2i is None:
        c2i = COLOR_TO_IDX
    p0, p1 = [], []
    for row in grid:
        b0 = b1 = 0
        for c, col in enumerate(row):
            if col is None or col == '.':
                continue
            idx = c2i[col]
            bit = 7 - c
            if idx & 1:
                b0 |= 1 << bit
            if idx & 2:
                b1 |= 1 << bit
        p0.append(b0)
        p1.append(b1)
    return p0, p1


def fmt(name, p):
    return "%s[8] = { %s };" % (name, ", ".join("0x%02X" % v for v in p))


def render(p0, p1):
    """Vuelve a dibujar el tile como ASCII (para comprobar)."""
    sym = {0: ' ', 1: '.', 2: '=', 3: '#'}
    out = []
    for r in range(8):
        out.append(''.join(sym[((((p1[r] >> (7 - c)) & 1) << 1) | ((p0[r] >> (7 - c)) & 1))]
                           for c in range(8)))
    return out


# Ejemplo: las dos mitades del bloque de ARKANOID (arte del usuario).
BLOCK_L = [
    [0xFFB10A09, 0xFFF8A7A6, 0xFFF8A7A6, 0xFFF8A7A6, 0xFFF8A7A6, 0xFFF8A7A6, 0xFFF8A7A6, 0xFFF8A7A6],
    [0xFFB10A09, 0xFFB10A09, 0xFFF8A7A6, 0xFFF8A7A6, 0xFFF8A7A6, 0xFFF8A7A6, 0xFFF8A7A6, 0xFFF8A7A6],
    [0xFFB10A09, 0xFFB10A09, 0xFFEF6E6D, 0xFFEF6E6D, 0xFFEF6E6D, 0xFFEF6E6D, 0xFFEF6E6D, 0xFFEF6E6D],
    [0xFFB10A09, 0xFFB10A09, 0xFFEF6E6D, 0xFFEF6E6D, 0xFFEF6E6D, 0xFFEF6E6D, 0xFFEF6E6D, 0xFFEF6E6D],
    [0xFFB10A09, 0xFFB10A09, 0xFFEF6E6D, 0xFFEF6E6D, 0xFFEF6E6D, 0xFFEF6E6D, 0xFFEF6E6D, 0xFFEF6E6D],
    [0xFFB10A09, 0xFFB10A09, 0xFFEF6E6D, 0xFFEF6E6D, 0xFFEF6E6D, 0xFFEF6E6D, 0xFFEF6E6D, 0xFFEF6E6D],
    [0xFFB10A09, 0xFFF8A7A6, 0xFFB10A09, 0xFFB10A09, 0xFFB10A09, 0xFFB10A09, 0xFFB10A09, 0xFFB10A09],
    [0xFFF8A7A6, 0xFFB10A09, 0xFFB10A09, 0xFFB10A09, 0xFFB10A09, 0xFFB10A09, 0xFFB10A09, 0xFFB10A09],
]
BLOCK_R = [
    [0xFFF8A7A6, 0xFFF8A7A6, 0xFFF8A7A6, 0xFFF8A7A6, 0xFFF8A7A6, 0xFFF8A7A6, 0xFFF8A7A6, 0xFFB10A09],
    [0xFFF8A7A6, 0xFFF8A7A6, 0xFFF8A7A6, 0xFFF8A7A6, 0xFFF8A7A6, 0xFFF8A7A6, 0xFFB10A09, 0xFFF8A7A6],
    [0xFFEF6E6D, 0xFFEF6E6D, 0xFFEF6E6D, 0xFFEF6E6D, 0xFFEF6E6D, 0xFFEF6E6D, 0xFFF8A7A6, 0xFFF8A7A6],
    [0xFFEF6E6D, 0xFFEF6E6D, 0xFFEF6E6D, 0xFFEF6E6D, 0xFFEF6E6D, 0xFFEF6E6D, 0xFFF8A7A6, 0xFFF8A7A6],
    [0xFFEF6E6D, 0xFFEF6E6D, 0xFFEF6E6D, 0xFFEF6E6D, 0xFFEF6E6D, 0xFFEF6E6D, 0xFFF8A7A6, 0xFFF8A7A6],
    [0xFFEF6E6D, 0xFFEF6E6D, 0xFFEF6E6D, 0xFFEF6E6D, 0xFFEF6E6D, 0xFFEF6E6D, 0xFFF8A7A6, 0xFFF8A7A6],
    [0xFFB10A09, 0xFFB10A09, 0xFFB10A09, 0xFFB10A09, 0xFFB10A09, 0xFFB10A09, 0xFFF8A7A6, 0xFFF8A7A6],
    [0xFFB10A09, 0xFFB10A09, 0xFFB10A09, 0xFFB10A09, 0xFFB10A09, 0xFFB10A09, 0xFFB10A09, 0xFFF8A7A6],
]


def main():
    for name, grid in (("tile_brick_l", BLOCK_L), ("tile_brick_r", BLOCK_R)):
        p0, p1 = planes(grid)
        print(fmt(name + "_p0", p0))
        print(fmt(name + "_p1", p1))
        for line in render(p0, p1):
            print("   " + line)
        print()


if __name__ == "__main__":
    main()
