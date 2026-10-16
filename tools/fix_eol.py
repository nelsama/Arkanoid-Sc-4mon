#!/usr/bin/env python3
"""Borra los finales de linea sobrantes: convierte LF solitarios en CRLF.

Los ficheros de este proyecto usan CRLF (Windows). Algunas ediciones dejan
lineas "mezcladas" (LF donde el resto es CRLF), lo que confunde al linker
(ld65) y al compilador (cl65). Este script lo normaliza.

Uso:  python tools/fix_eol.py game.c README.md ...
"""
import sys

for path in sys.argv[1:]:
    with open(path, "rb") as f:
        data = f.read()
    fixed = data.replace(b"\r\n", b"\n").replace(b"\n", b"\r\n")
    if fixed != data:
        with open(path, "wb") as f:
            f.write(fixed)
        print("normalizado:", path)
    else:
        print("ya estaba bien:", path)
