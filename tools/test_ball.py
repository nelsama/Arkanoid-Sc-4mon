#!/usr/bin/env python3
"""
test_ball.py - Simulacion de la fisica de la bola de ARKANOID (para PC).

NO forma parte del juego. Reproduce la aritmetica Q8.8 de ball_step() de game.c
para verificar que la bola nunca se sale del campo. El bug original era que el
clamp del borde derecho hacia (256 << 8) en un uint16_t -> 0, y la bola saltaba
al lado izquierdo.

Uso:  python tools/test_ball.py
"""

# --- Constantes copiadas de game.c (mantener en sincronia) ---
FIELD_TOP    = 3
FIELD_BOTTOM = 27
FIELD_LEFT   = 7
FIELD_RIGHT  = 32
BALL_SIZE    = 6

PLAY_X_MIN = (FIELD_LEFT + 1) * 8            # 64
PLAY_X_MAX = FIELD_RIGHT * 8 - 1             # 255
PLAY_Y_MIN = (FIELD_TOP + 1) * 8             # 32
PLAY_Y_MAX = FIELD_BOTTOM * 8 - 1            # 215
BALL_X_MAX = PLAY_X_MAX - BALL_SIZE + 1      # 248

BRICK_W    = 2
BRICK_COLS = (FIELD_RIGHT - FIELD_LEFT + 1 - 2) // BRICK_W
BRICK_ROWS = 5
BRICK_X0   = FIELD_LEFT + 1
BRICK_Y0   = 6

U16 = 0xFFFF


def brick_at(brick, col, row):
    if col < BRICK_X0 or col >= BRICK_X0 + BRICK_COLS * BRICK_W:
        return 0
    if row < BRICK_Y0 or row >= BRICK_Y0 + BRICK_ROWS:
        return 0
    return brick[row - BRICK_Y0][(col - BRICK_X0) // BRICK_W]


def solid_px(brick, px, py):
    col, row = px >> 3, py >> 3
    if col == FIELD_LEFT or col == FIELD_RIGHT:
        return True
    if row == FIELD_TOP:
        return True
    return brick_at(brick, col, row) != 0


def ball_step(st):
    """Replica de ball_step() de game.c. Devuelve 1 si la bola cae (vida perdida)."""
    st['frac'] = (st['frac'] + (st['speed'] & 0xFF)) & U16
    steps = st['speed'] >> 8
    if st['frac'] >= 0x100:
        steps += 1
        st['frac'] -= 0x100
    if steps == 0:
        steps = 1

    for _ in range(steps):
        x = st['x'] >> 8
        y = st['y'] >> 8
        newx = (x + 1) if st['dx'] else (x - 1)
        newy = (y + 1) if st['dy'] else (y - 1)
        lx, rx = newx, newx + BALL_SIZE - 1
        ty, byy = newy, newy + BALL_SIZE - 1
        cy = y + BALL_SIZE // 2
        cx = x + BALL_SIZE // 2

        # --- Eje X ---
        edge_x = rx if st['dx'] else lx
        if solid_px(st['brick'], edge_x, cy):
            st['dx'] ^= 1
            lx, rx = x, x + BALL_SIZE - 1

        # --- Eje Y ---
        edge_y = byy if st['dy'] else ty
        if solid_px(st['brick'], lx, edge_y) or solid_px(st['brick'], rx, edge_y):
            st['dy'] ^= 1
            ty, byy = y, y + BALL_SIZE - 1

        # --- Aplica movimiento ---
        st['x'] = (st['x'] + (0x100 if st['dx'] else -0x100)) & U16
        st['y'] = (st['y'] + (0x100 if st['dy'] else -0x100)) & U16

        # --- Clamp ---
        if (st['x'] >> 8) < PLAY_X_MIN:
            st['x'] = (PLAY_X_MIN << 8) & U16
            st['dx'] = 1
        elif (st['x'] >> 8) > BALL_X_MAX:
            st['x'] = (BALL_X_MAX << 8) & U16
            st['dx'] = 0
        if (st['y'] >> 8) < PLAY_Y_MIN:
            st['y'] = (PLAY_Y_MIN << 8) & U16
            st['dy'] = 1

        st['min_x'] = min(st['min_x'], st['x'] >> 8)
        st['max_x'] = max(st['max_x'], st['x'] >> 8)
        st['min_y'] = min(st['min_y'], st['y'] >> 8)
        st['max_y'] = max(st['max_y'], st['y'] >> 8)

        if (st['x'] >> 8) < PLAY_X_MIN or (st['x'] >> 8) > BALL_X_MAX:
            st['oob'] = True

        if (st['y'] >> 8) > PLAY_Y_MAX:
            return 1
    return 0


def new_state(dx, dy, level):
    return {
        'brick': [[r + 1 for _ in range(BRICK_COLS)] for r in range(BRICK_ROWS)],
        'x': 130 << 8, 'y': 200 << 8,
        'dx': dx, 'dy': dy,
        'speed': 0x100 + (level - 1) * 0x40,
        'frac': 0,
        'min_x': 99999, 'max_x': -1, 'min_y': 99999, 'max_y': -1,
        'oob': False,
    }


def main():
    bad = False
    for dx, dy in ((1, 0), (0, 1), (1, 1), (0, 0)):
        for level in range(1, 10):
            st = new_state(dx, dy, level)
            for _ in range(200000):
                if ball_step(st) == 1:
                    break
                if st['oob']:
                    break
            ok = (st['min_x'] >= PLAY_X_MIN and st['max_x'] <= BALL_X_MAX and not st['oob'])
            print(f"dir({dx},{dy}) nivel {level}: "
                  f"x[{st['min_x']}..{st['max_x']}] y[{st['min_y']}..{st['max_y']}] "
                  f"{'OK' if ok else 'FALLO'}{'  <-- OOB!' if st['oob'] else ''}")
            if not ok:
                bad = True
    print(f"\n{'HAY FALLOS' if bad else 'TODO OK'} "
          f"(rango valido ball_x: {PLAY_X_MIN}..{BALL_X_MAX})")
    return 1 if bad else 0


if __name__ == "__main__":
    raise SystemExit(main())
