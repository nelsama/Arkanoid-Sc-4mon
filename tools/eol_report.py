#!/usr/bin/env python3
"""Muestra el "ancho de linea" del juego y los bloques de lineas modificados.

cc65 y ld65 se lian si el MISMO fichero mezcla finales de linea LF y CRLF.
Este script cuenta cuantas lineas terminan en CRLF y cuantas en LF, y lista los
tramos de lineas que no siguen la mayoria (util para localizar el tramo roto).

Uso:  python tools/eol_report.py game.c
"""
import sys

path = sys.argv[1] if len(sys.argv) > 1 else "game.c"
data = open(path, "rb").read()
lines = data.split(b"\n")

mix = [(i + 1, l.endswith(b"\r")) for i, l in enumerate(lines) if l]
lf = [n for n, c in mix if not c]
print("%s: %d lineas, %d con CRLF, %d con LF" % (path, len(mix), len(mix) - len(lf), len(lf)))
if lf:
    print("lineas sin CR:", lf)
