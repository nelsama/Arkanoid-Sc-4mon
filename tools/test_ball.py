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
BALL_X_MAX = PLAY_X_MAX - BALL_SIZE + 1      # 255 - 6 + 1 = 250

# Pala (para el test de rebote en la pala; coincide con game.c)
PADDLE_Y   = 206
PADDLE_H   = 8

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


def solid_kind(brick, px, py):
    """Replica de solid_kind() de game.c: 0 = libre, 1 = muro, 2 = ladrillo."""
    col, row = px >> 3, py >> 3
    if col == FIELD_LEFT or col == FIELD_RIGHT:
        return 1
    if row == FIELD_TOP:
        return 1
    return 2 if brick_at(brick, col, row) else 0


def ball_step(st):
    """Replica de ball_step() de game.c. Devuelve 1 si la bola cae (vida perdida).

    Modelo ACTUAL: velocidad por eje (vx/vy Q8.8 CON SIGNO). El signo es la
    direccion, asi que dx/dy se derivan de vx/vy para que el resto del test siga
    valiendo. Cuando el test se escribio, la fisica llevaba dx/dy separados.
    """
    st['frac_x'] = (st['frac_x'] + (st['vx'] & 0xFF)) & U16
    # OJO: los pasos salen de la MAGNITUD. Con (st['vx'] & 0xFFFF) >> 8 un vx
    # negativo daria 254 pasos en un frame (el byte alto de 0xFFB0 es 0xFF).
    steps_x = (abs(st['vx']) >> 8)
    if st['frac_x'] >= 0x100:
        steps_x += 1
        st['frac_x'] -= 0x100

    st['frac_y'] = (st['frac_y'] + (st['vy'] & 0xFF)) & U16
    steps_y = (abs(st['vy']) >> 8)
    if st['frac_y'] >= 0x100:
        steps_y += 1
        st['frac_y'] -= 0x100

    sx, sy = steps_x, steps_y
    total = max(steps_x, steps_y) or 1

    st['hit_paddle'] = False
    done = False

    for _ in range(total):
        x = st['x'] >> 8
        y = st['y'] >> 8
        cy = y + BALL_SIZE // 2
        # El DIRECCION se lee de vx/vy EN CADA PASO: un rebote cambia el signo y
        # con el hacia donde se mueve y donde esta el borde delantero.
        st['dx'] = 1 if st['vx'] >= 0 else 0
        st['dy'] = 1 if st['vy'] >= 0 else 0
        # Bordes del intento actual; se recalculan en cada eje.
        lx, rx = x, x + BALL_SIZE - 1
        ty, byy = y, y + BALL_SIZE - 1

        # --- Eje X (solo si le quedan pasos) ---
        # Las esquinas delanteras se calculan AQUI, con el signo ya actualizado:
        # un rebote en un eje cambia hacia donde esta "delante" en ese eje.
        if sx:
            newx = (x + 1) if st['dx'] else (x - 1)
            lx, rx = newx, newx + BALL_SIZE - 1
            edge_x = rx if st['dx'] else lx
            k = solid_kind(st['brick'], edge_x, cy)
            if k:
                if k == 2:
                    # rompe el ladrillo (brick_break): la celda deja de ser solida
                    _break(st, edge_x, cy)
                st['vx'] = -st['vx']
                lx, rx = x, x + BALL_SIZE - 1

        # --- Eje Y (solo si le quedan pasos) ---
        if sy:
            newy = (y + 1) if st['dy'] else (y - 1)
            ty, byy = newy, newy + BALL_SIZE - 1
            edge_y = byy if st['dy'] else ty
            kL = solid_kind(st['brick'], lx, edge_y)
            kR = 0 if kL else solid_kind(st['brick'], rx, edge_y)
            k = kL or kR
            if k:
                if k == 2:
                    _break(st, lx if kL else rx, edge_y)
                st['vy'] = -st['vy']
                ty, byy = y, y + BALL_SIZE - 1

        # --- Pala (solo si la bola baja) ---
        if newy >= 0 and st['vy'] > 0:
            pl = newy + BALL_SIZE - 1
            if ((pl + 1) > PADDLE_Y and newy < PADDLE_Y + PADDLE_H and
                    (rx + 1) > st['paddle_x'] and
                    lx < st['paddle_x'] + st['paddle_w']):
                rel = (lx + BALL_SIZE // 2) - st['paddle_x']
                if rel < 0:
                    rel = 0
                zone = 0
                for cut in (6, 12, 18):
                    if rel >= cut:
                        zone += 1
                st['vx'] = (-0x180, -0x0B0, 0x0B0, 0x180)[zone]
                st['vy'] = -st['vy_base']
                # La bola se apoya JUSTO ENCIMA de la pala (no dentro, o el
                # siguiente frame vuelve a rebotar y se "baila" sobre ella).
                st['y'] = PADDLE_Y << 8
                st['hit_paddle'] = True
                done = True   # el impacto consume el frame: fuera del bucle
                break

        # --- Aplica movimiento de los ejes que tengan paso ---
        if sx:
            st['x'] = (st['x'] + (0x100 if st['dx'] else -0x100)) & U16
            sx -= 1
        if sy:
            st['y'] = (st['y'] + (0x100 if st['dy'] else -0x100)) & U16
            sy -= 1

        # --- Clamp ---
        if (st['x'] >> 8) < PLAY_X_MIN:
            st['x'] = (PLAY_X_MIN << 8) & U16
            st['vx'] = abs(st['vx'])
        elif (st['x'] >> 8) > BALL_X_MAX:
            st['x'] = (BALL_X_MAX << 8) & U16
            st['vx'] = -abs(st['vx'])
        if (st['y'] >> 8) < PLAY_Y_MIN:
            st['y'] = (PLAY_Y_MIN << 8) & U16
            st['vy'] = abs(st['vy'])

        st['min_x'] = min(st['min_x'], st['x'] >> 8)
        st['max_x'] = max(st['max_x'], st['x'] >> 8)
        st['min_y'] = min(st['min_y'], st['y'] >> 8)
        st['max_y'] = max(st['max_y'], st['y'] >> 8)

        if (st['x'] >> 8) < PLAY_X_MIN or (st['x'] >> 8) > BALL_X_MAX:
            st['oob'] = True

        if (st['y'] >> 8) > PLAY_Y_MAX:
            return 1

        if done:      # rebote en la pala: el impacto ya gasto el frame
            break
    return 0


def _break(st, px, py):
    """Replica de break_px(): vacia la celda de ladrillo en la que cae (px,py)."""
    col, row = px >> 3, py >> 3
    if (BRICK_X0 <= col < BRICK_X0 + BRICK_COLS * BRICK_W and
            BRICK_Y0 <= row < BRICK_Y0 + BRICK_ROWS):
        st['brick'][row - BRICK_Y0][(col - BRICK_X0) // BRICK_W] = 0


def _fmt_v(v):
    """Formatea una velocidad Q8.8 con signo (0x0580 -> +5.50 px/f)."""
    return f"{'-' if v < 0 else '+'}{abs(v) / 256:.2f}"


def new_state(dx, dy, level, vx=None, vy=None, paddle_x=130, paddle_w=24):
    """Estado inicial. Si no se dan vx/vy se usan los valores de la fisica vieja
    (velocidad igual en los dos ejes = 45 grados)."""
    speed = 0x100 + (level - 1) * 0x40   # solo para el modo viejo
    return {
        'brick': [[r + 1 for _ in range(BRICK_COLS)] for r in range(BRICK_ROWS)],
        'x': 130 << 8, 'y': 200 << 8,
        'dx': dx, 'dy': dy,
        'vx': speed if vx is None else vx,
        'vy': speed if vy is None else vy,
        'frac_x': 0, 'frac_y': 0,
        'paddle_x': paddle_x, 'paddle_w': paddle_w,
        'min_x': 99999, 'max_x': -1, 'min_y': 99999, 'max_y': -1,
        'oob': False, 'lost': False,
    }


def main():
    bad = False

    # --- Escenario 1: la bola vuela libre (nunca toca la pala de prueba) ---
    # Velocidades por eje que puede generar el juego en el nivel 1 (ball_vy=0x180):
    #   (0x180,0x180) = rebote en las puntas (45 grados), la mas rapida
    #   (0x0B0,0x180) = rebote suave (25 grados)
    #   (0,0x180)     = rebote de esquina / lanzamiento recto
    casos = [(1, 0, 0x180, 0x180), (0, 1, 0x0B0, 0x180),
             (1, 1, 0, 0x180), (0, 0, 0x180, 0x180)]
    for dx, dy, vx, vy in casos:
        for level in range(1, 10):
            sc = 0x180 + (level - 1) * 0x80   # escala del nivel (ball_vy)
            st = new_state(dx, dy, level,
                           vx=(vx * sc) // 0x180, vy=(vy * sc) // 0x180)
            st['vy_base'] = abs(st['vy'])
            for _ in range(200000):
                if ball_step(st) == 1:
                    break
                if st['oob']:
                    break
            ok = (st['min_x'] >= PLAY_X_MIN and st['max_x'] <= BALL_X_MAX and not st['oob'])
            print(f"v({_fmt_v(st['vx'])},{_fmt_v(st['vy'])})px/f nivel {level}: "
                  f"x[{st['min_x']}..{st['max_x']}] y[{st['min_y']}..{st['max_y']}] "
                  f"{'OK' if ok else 'FALLO'}{'  <-- OOB!' if st['oob'] else ''}")
            if not ok:
                bad = True

    # --- Escenario 2: rebote en la pala (el bug del 2026-10-16) ---
    # La bola caia SOBRE la pala y el test de colision miraba la celda de ABAJO
    # (el fondo), que es solida: la bola rebotaba contra el suelo y se perdia en
    # 2 frames. Aqui se comprueba que la bola SALE HACIA ARRIBA.
    #
    # CLAVE: la bola cae en VERTICAL (vx = 0) sobre el centro de una pala ancha.
    # Asi no se sale por el lado de la pala mientras cae y el test aisla el bug
    # de "hacia donde esta delante" (que es el que rompia el rebote).
    print("\n--- Rebote en la pala (debe salir hacia arriba) ---")
    for level in range(1, 10):
        sc = 0x180 + (level - 1) * 0x80
        vy = (0x180 * sc) // 0x180
        st = new_state(0, 1, level, vx=0, vy=vy, paddle_x=112, paddle_w=32)
        st['vy_base'] = vy
        st['y'] = (PADDLE_Y - 40) << 8            # cae de mas arriba
        st['x'] = (112 + 32 // 2 - BALL_SIZE // 2) << 8   # centrada en la pala
        st['hit_paddle'] = False
        # El jugador MUEVE la pala para seguir la bola (si no, la bola cae al lado
        # y se pierde sin que sea un fallo). Esto reproduce el sintoma real:
        # pulsar el boton, la bola sale y hay que recogerla con la pala.
        st['paddle_x'] = (st['x'] >> 8) + BALL_SIZE // 2 - st['paddle_w'] // 2
        bounce_frame = 0
        bounces = 0
        lost = 0
        for frame in range(3000):
            # Pala pegada a la bola (solo el centrado: PADDLE_SPEED da igual).
            st['paddle_x'] = (st['x'] >> 8) + BALL_SIZE // 2 - st['paddle_w'] // 2
            st['paddle_x'] = max(PLAY_X_MIN,
                                 min(st['paddle_x'],
                                     PLAY_X_MAX - st['paddle_w'] + 1))
            st['hit_paddle'] = False
            if ball_step(st) == 1:
                lost = frame + 1
                break
            if st['hit_paddle']:
                bounces += 1
                if bounces == 1:
                    bounce_frame = frame + 1
                if st['vy'] > 0:
                    print(f"nivel {level}: FALLO - el rebote deja vy={_fmt_v(st['vy'])}")
                    bad = True
                    break
        ok = bounces >= 5
        print(f"nivel {level}: rebotes={bounces} primero en frame {bounce_frame} "
              f"{'OK' if ok else 'FALLO: se pierde sin rebotar bien'}"
              f"{'  <-- PERDIDA en frame ' + str(lost) if lost else ''}")
        if not ok:
            bad = True

    # --- Escenario 3: el LANZAMIENTO real (bola apoyada en la pala) ---
    # Es el sintoma que reporto el usuario: pulsar el boton y la bola no sale.
    # Estado de paddle_reset(): la bola descansa sobre la pala con vy negativa y
    # vx positiva (ball_vy repartido). Antes del arreglo, la bola rebotaba varias
    # veces SEGUIDAS encima de la pala (se "bailaba") y salia por el lado.
    print("\n--- Lanzamiento real (bola apoyada en la pala) ---")
    for level in range(1, 10):
        sc = 0x180 + (level - 1) * 0x80
        vy = (0x180 * sc) // 0x180
        vx = (0x0B0 * sc) // 0x180
        st = new_state(1, 1, level, vx=vx, vy=-vy, paddle_x=147, paddle_w=24)
        st['vy_base'] = vy
        st['x'] = (147 + 24 // 2 - BALL_SIZE // 2) << 8
        st['y'] = (PADDLE_Y - BALL_SIZE) << 8
        st['hit_paddle'] = False
        first = 0
        bounce_frame = 0
        re_bounces = 0
        for frame in range(60):
            was_hit = st['hit_paddle']
            st['hit_paddle'] = False
            tb_hit = False
            if ball_step(st) == 1:
                break
            if st['hit_paddle']:
                # Un rebote "encadenado" = segundo rebote sin que la bola haya
                # llegado a alejarse y volver (menos de 10 frames desde el otro).
                if bounce_frame and frame - bounce_frame < 10:
                    re_bounces += 1
                bounce_frame = frame
        ok = (st['y'] < (PADDLE_Y - BALL_SIZE) << 8 and re_bounces == 0)
        print(f"nivel {level}: re-rebotes encadenados={re_bounces} "
              f"altura final={st['y'] >> 8} {'OK' if ok else 'FALLO: se baila encima de la pala'}")
        if not ok:
            bad = True

    print(f"\n{'HAY FALLOS' if bad else 'TODO OK'} "
          f"(rango valido ball_x: {PLAY_X_MIN}..{BALL_X_MAX})")
    return 1 if bad else 0


if __name__ == "__main__":
    raise SystemExit(main())
