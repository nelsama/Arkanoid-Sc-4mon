#!/usr/bin/env python3
"""Genera los planos (p0/p1) de las capsulas de ARKANOID a partir de rejillas ASCII.

Convencion de indices del core:  idx = (plano1<<1)|plano0
  '#' = 3 (claro), '=' = 2 (medio), '.' = 1 (oscuro), ' ' = 0 (transparente)
"""

# Letras 3x5 (1 = trazo)
LETTERS = {
    'E': ['111', '100', '110', '100', '111'],
    'N': ['101', '111', '111', '101', '101'],
    'C': ['011', '100', '100', '100', '011'],
    'L': ['100', '100', '100', '100', '111'],
    'D': ['110', '101', '101', '101', '110'],
    'S': ['011', '100', '010', '001', '110'],
    'B': ['110', '101', '110', '101', '110'],
    'P': ['110', '101', '110', '100', '100'],
}


def capsule(ch):
    """Capsula 8x8: marco claro (filas 1..6, cols 0..7 con esquinas cortadas),
    relleno medio y letra 3x5 en claro."""
    out = []
    for r in range(8):
        row = ''
        for c in range(8):
            inside = (1 <= r <= 6) and (0 <= c <= 7)
            if inside and (r in (1, 6)) and (c in (0, 7)):
                inside = False
            if not inside:
                row += ' '
                continue
            rr, cc = r - 2, c - 3
            lit = (0 <= rr < 5 and 0 <= cc < 3 and LETTERS[ch][rr][cc] == '1')
            border = (r in (1, 6)) or (c in (0, 7))
            row += '#' if (lit or border) else '='
        out.append(row)
    return out


IDX = {' ': 0, '.': 1, '=': 2, '#': 3}


def planes(grid):
    p0, p1 = [], []
    for row in grid:
        b0 = b1 = 0
        for c in range(8):
            idx = IDX[row[c]]
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


def main():
    for ch in ['E', 'N', 'C', 'L', 'D', 'S', 'B', 'P']:
        g = capsule(ch)
        p0, p1 = planes(g)
        print(f"/* Capsula {ch} */")
        print(fmt(f"caps_{ch}_p0", p0))
        print(fmt(f"caps_{ch}_p1", p1))
        for r in g:
            print("   |" + r + "|")
        print()


if __name__ == "__main__":
    main()
