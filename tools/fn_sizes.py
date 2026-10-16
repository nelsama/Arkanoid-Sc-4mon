#!/usr/bin/env python3
"""Tamano (en bytes y en lineas) de cada funcion del listado de cc65.

Uso:  python tools/fn_sizes.py build/game.lst
"""
import re
import sys

path = sys.argv[1] if len(sys.argv) > 1 else "build/game.lst"
LINE = re.compile(r"^([0-9A-Fa-f]+)r?\s+1\s+(?:[0-9A-Fa-f]{2}\s+){1,}[0-9A-Fa-f]{2}\s")

name = None
start = None
last = None
nlines = 0
rows = []

for ln in open(path, "r", errors="replace").read().split("\n"):
    m = LINE.match(ln)
    addr = int(m.group(1), 16) if m else None
    if ".proc" in ln:
        name = ln.split(".proc", 1)[1].split(":", 1)[0].strip()
        start = last = None
        nlines = 0
        continue
    if addr is not None:
        if start is None:
            start = addr
        last = addr
        nlines += 1
    if addr is None and ".endproc" in ln:
        if name and start is not None and last is not None:
            rows.append((name, last - start, nlines))
        name = start = last = None

if not rows:
    print("No se encontro ninguna funcion. Secciones del listado:")
    for ln in open(path, "r", errors="replace").read().split("\n"):
        if ".proc" in ln or ".segment" in ln:
            print("   ", ln.strip())

rows.sort(key=lambda r: -r[1])
for n, s, l in rows:
    print("%-22s %5d bytes  %4d lineas" % (n, s, l))
print("----- %d funciones, %d bytes" % (len(rows), sum(r[1] for r in rows)))
