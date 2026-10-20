#!/usr/bin/env python3
"""
make_gfx.py - build the Game Boy tile data and view-cone tables for the stealth prototype.

    python3 tools/make_gfx.py assets/hero.png src/gfx.c src/gfx.h

Art key: ' ' transparent / colour 0, '.' colour 0, '+' colour 1, '=' colour 2, '#' colour 3.

Hero (player and NPCs): "8-Directional Game Boy Character Template" by GibbonGL (CC0 1.0),
https://gibbongl.itch.io/8-directional-gameboy-character-template
(same extraction as in Tether). NPCs use the same tiles with a darker sprite palette.

Sprite tiles (0x8000, 8x16 mode):
    0..95    hero: 24 frames x 4 tiles, hero_anim[dir][step] -> frame
    96..99   lying (taken down / killed)
    100..101 '?' icon    102..103 '!' icon
Background tiles (0x9000 region, signed indexing):
    0..2 ground outside, 3..4 floor inside, 5 wall, 6..47 font (light on dark)
    128..255 (0x8800) cone tile pool, filled at run time.

View cones: for each of 32 facing angles (11.25 degrees apart, 0 = east, clockwise on screen
because y points down) a cone CONE_LEN long and CONE_DEG wide, relative to the NPC's eye,
stored as one pixel span per row. Walls block the view per 8x8 cell, using a table of the
cells that lie between two cells (cell_los).
"""

import math
import sys
from PIL import Image

CONE_LEN = 60       # px
CONE_DEG = 30       # full angle
CONE_ANGLES = 64
LOS_R = 8           # line-of-sight table: cells within +-LOS_R of the eye cell (>= CONE_LEN / 8 + 1)
PW = 22             # padded map width in the game (src/game.h)

CH = {" ": 0, ".": 0, "+": 1, "=": 2, "#": 3}


def grid_from_art(rows, w, h, ox=0, oy=0):
    """ASCII art -> h x w grid of colour indices placed at (ox, oy)."""
    g = [[0] * w for _ in range(h)]
    for y, r in enumerate(rows):
        for x, c in enumerate(r):
            if 0 <= x + ox < w and 0 <= y + oy < h:
                g[y + oy][x + ox] = CH[c]
    return g


def tile_bytes(g, x0=0, y0=0):
    out = []
    for y in range(y0, y0 + 8):
        lo = hi = 0
        for x in range(x0, x0 + 8):
            c = g[y][x]
            lo = (lo << 1) | (c & 1)
            hi = (hi << 1) | ((c >> 1) & 1)
        out += [lo, hi]
    return out


def sprite16(g):
    """16x16 grid -> 4 tiles in 8x16 order: LT, LB, RT, RB."""
    return tile_bytes(g, 0, 0) + tile_bytes(g, 0, 8) + tile_bytes(g, 8, 0) + tile_bytes(g, 8, 8)


def sprite8x16(g):
    """8x16 grid -> 2 tiles: top, bottom."""
    return tile_bytes(g, 0, 0) + tile_bytes(g, 0, 8)


def bg16(g):
    """16x16 grid -> 4 tiles in map order: TL, TR, BL, BR."""
    return tile_bytes(g, 0, 0) + tile_bytes(g, 8, 0) + tile_bytes(g, 0, 8) + tile_bytes(g, 8, 8)


# ---------------------------------------------------------------- hero
HERO_BG = (48, 104, 80)
HERO_COL = {(224, 248, 207): 1, (134, 192, 108): 2, (7, 24, 33): 3}


def hero_cell(img, row, col):
    x0, y0 = col * 16, row * 16
    rgb = [[img.getpixel((x0 + x, y0 + y))[:3] for x in range(16)] for y in range(16)]
    bg = [[False] * 16 for _ in range(16)]
    st = [(x, y) for x in range(16) for y in (0, 15)] + [(x, y) for y in range(16) for x in (0, 15)]
    while st:
        x, y = st.pop()
        if 0 <= x < 16 and 0 <= y < 16 and not bg[y][x] and rgb[y][x] == HERO_BG:
            bg[y][x] = True
            st += [(x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)]
    return [[0 if bg[y][x] else HERO_COL[rgb[y][x]] for x in range(16)] for y in range(16)]



FONT_CHARS = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!,.-:"
FONT = {
    "A": [".###.", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"],
    "B": ["####.", "#...#", "#...#", "####.", "#...#", "#...#", "####."],
    "C": [".###.", "#...#", "#....", "#....", "#....", "#...#", ".###."],
    "D": ["####.", "#...#", "#...#", "#...#", "#...#", "#...#", "####."],
    "E": ["#####", "#....", "#....", "####.", "#....", "#....", "#####"],
    "F": ["#####", "#....", "#....", "####.", "#....", "#....", "#...."],
    "G": [".###.", "#...#", "#....", "#.###", "#...#", "#...#", ".####"],
    "H": ["#...#", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"],
    "I": [".###.", "..#..", "..#..", "..#..", "..#..", "..#..", ".###."],
    "J": ["..###", "...#.", "...#.", "...#.", "...#.", "#..#.", ".##.."],
    "K": ["#...#", "#..#.", "#.#..", "##...", "#.#..", "#..#.", "#...#"],
    "L": ["#....", "#....", "#....", "#....", "#....", "#....", "#####"],
    "M": ["#...#", "##.##", "#.#.#", "#.#.#", "#...#", "#...#", "#...#"],
    "N": ["#...#", "#...#", "##..#", "#.#.#", "#..##", "#...#", "#...#"],
    "O": [".###.", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."],
    "P": ["####.", "#...#", "#...#", "####.", "#....", "#....", "#...."],
    "Q": [".###.", "#...#", "#...#", "#...#", "#.#.#", "#..#.", ".##.#"],
    "R": ["####.", "#...#", "#...#", "####.", "#.#..", "#..#.", "#...#"],
    "S": [".####", "#....", "#....", ".###.", "....#", "....#", "####."],
    "T": ["#####", "..#..", "..#..", "..#..", "..#..", "..#..", "..#.."],
    "U": ["#...#", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."],
    "V": ["#...#", "#...#", "#...#", "#...#", "#...#", ".#.#.", "..#.."],
    "W": ["#...#", "#...#", "#...#", "#.#.#", "#.#.#", "#.#.#", ".#.#."],
    "X": ["#...#", "#...#", ".#.#.", "..#..", ".#.#.", "#...#", "#...#"],
    "Y": ["#...#", "#...#", ".#.#.", "..#..", "..#..", "..#..", "..#.."],
    "Z": ["#####", "....#", "...#.", "..#..", ".#...", "#....", "#####"],
    "0": [".###.", "#...#", "#..##", "#.#.#", "##..#", "#...#", ".###."],
    "1": ["..#..", ".##..", "..#..", "..#..", "..#..", "..#..", ".###."],
    "2": [".###.", "#...#", "....#", "...#.", "..#..", ".#...", "#####"],
    "3": ["####.", "....#", "....#", ".###.", "....#", "....#", "####."],
    "4": ["...#.", "..##.", ".#.#.", "#..#.", "#####", "...#.", "...#."],
    "5": ["#####", "#....", "####.", "....#", "....#", "#...#", ".###."],
    "6": ["..##.", ".#...", "#....", "####.", "#...#", "#...#", ".###."],
    "7": ["#####", "....#", "...#.", "..#..", ".#...", ".#...", ".#..."],
    "8": [".###.", "#...#", "#...#", ".###.", "#...#", "#...#", ".###."],
    "9": [".###.", "#...#", "#...#", ".####", "....#", "...#.", ".##.."],
    "!": ["..#..", "..#..", "..#..", "..#..", "..#..", ".....", "..#.."],
    ",": [".....", ".....", ".....", ".....", ".##..", "..#..", ".#..."],
    ".": [".....", ".....", ".....", ".....", ".....", ".##..", ".##.."],
    "-": [".....", ".....", ".....", "#####", ".....", ".....", "....."],
    ":": [".....", ".##..", ".##..", ".....", ".##..", ".##..", "....."],
}


def glyph(ch):
    """Light text (colour 0) on the dark HUD background (colour 3)."""
    g = [[3] * 8 for _ in range(8)]
    for y, row in enumerate(FONT.get(ch, ["....."] * 7)):
        for x, c in enumerate(row):
            if c == "#":
                g[y][x + 1] = 0
    return tile_bytes(g)




ART_QUESTION = [                  # '+' = white fill (sprite colour 1), '.' would be transparent
    " ####  ",
    "##++## ",
    "#+##+# ",
    "####+# ",
    "  #+## ",
    "  #+#  ",
    "  ###  ",
    "  #+#  ",
    "  ###  "]
ART_BANG = [
    "  ###  ",
    "  #+#  ",
    "  #+#  ",
    "  #+#  ",
    "  #+#  ",
    "  ###  ",
    "  #+#  ",
    "  ###  "]


def ground_tiles():
    """Outside: colour 0 with a few colour-1 grass tufts. Floor tiles use colours 0 and 1 only:
    the cone is ORed into the low bitplane, so their high plane must stay 0."""
    tiles = []
    for tufts in ([(1, 2), (5, 6)], [(4, 1)], []):
        g = [[0] * 8 for _ in range(8)]
        for x, y in tufts:
            g[y][x] = 1
            g[y][x + 1] = 1
            g[y - 1][x + 1] = 1
        tiles.append(tile_bytes(g))
    return tiles


def floor_tiles():
    """Inside: wide planks in colour 0 with faint colour-1 joints; the seam line is only on
    the second tile, which the map uses on every other row."""
    plain = [[0] * 8 for _ in range(8)]
    for y in (2, 3, 4):
        plain[y][5] = 1
    seam = [[0] * 8 for _ in range(8)]
    for x in range(8):
        seam[7][x] = 1
    for y in (2, 3, 4):
        seam[y][1] = 1
    return [tile_bytes(plain), tile_bytes(seam)]


def wall_tile():
    g = [[2] * 8 for _ in range(8)]
    for x in range(8):
        g[0][x] = 3
        g[4][x] = 3
    for y in (1, 2, 3):
        g[y][3] = 3
    for y in (5, 6, 7):
        g[y][7] = 3
    return tile_bytes(g)


def pit_tiles():
    """Ravine (walls you can see across): black with a few stones, and a lit rock face on each
    side that borders open ground. Index = up 1 | left 2 | right 4 | down 8 (open neighbours)."""
    tiles = []
    for m in range(16):
        up, left, right, down = m & 1, m & 2, m & 4, m & 8
        g = [[3] * 8 for _ in range(8)]
        for y in range(8):
            for x in range(8):
                if (up and y < 2) or (left and x < 1) or (right and x > 6) or (down and y > 6):
                    g[y][x] = 2
                elif (x * 7 + y * 13 + 5) % 23 == 0:
                    g[y][x] = 2
        tiles.append(tile_bytes(g))
    return tiles


def arrow_tiles():
    """Level select arrows, light on the dark title background: left, right."""
    out = []
    for right in (False, True):
        g = [[3] * 8 for _ in range(8)]
        for k in range(4):
            x = 2 + k if not right else 5 - k
            for y in range(4 - k, 5 + k):
                g[y][x] = 0
        out.append(tile_bytes(g))
    return out


def exit_sprites():
    """Exit marker, 16 x 16 (two 8x16 sprites): a dashed frame 16 x 10 with two chevrons
    pointing out of the level (two drift positions), or crossed bars when locked."""
    frames = []
    for kind in ("open0", "open1", "locked"):
        g = [[0] * 16 for _ in range(16)]
        for i in range(0, 16, 2):
            g[0][i] = g[9][i] = 3
        for i in range(0, 10, 2):
            g[i][0] = g[i][15] = 3
        cx, cy = 8, 5
        if kind == "locked":
            for k in range(-3, 4):
                g[cy + k][cx + k] = 3
                g[cy - k][cx + k] = 3
        else:
            off = 0 if kind == "open0" else 1
            for dy in (-2 - off, 2 - off):
                for k in range(3):
                    for x in (cx - k, cx + k):
                        y = cy + dy + k
                        if 0 < y < 9 and 0 < x < 15:
                            g[y][x] = 3
        frames.append(sprite16(g))
    return frames


def bar_sprites():
    """Knock-out timer bar, 8 px wide, in the top rows of an 8x16 sprite (sprite palette 1):
    black border, light inside, dark fill of 0..6 px."""
    out = []
    for n in range(7):
        g = [[0] * 8 for _ in range(16)]
        for x in range(8):
            g[0][x] = g[3][x] = 3
        for y in (1, 2):
            g[y][0] = g[y][7] = 3
            for x in range(1, 7):
                g[y][x] = 2 if x - 1 < n else 1
        out.append(sprite8x16(g))
    return out


# ---------------------------------------------------------------- items, bushes (stage 3)
ITEM_ORDER = ["shuriken", "rope", "trap", "stone", "coin"]
ITEM_ART = {   # 8 x 8, digits = colour, '.' = see-through (as in the browser prototype)
    "shuriken": ["...3....", "...33...", "3.3223..", ".32..233", "332..23.", "..3223.3", "...33...", "....3..."],
    "rope":     ["..3333..", ".322223.", "32.33.23", "323..323", "323..323", "32.33.23", ".322223.", "..3333.3"],
    "trap":     ["........", "3.3.3.3.", "32323232", "32222223", "3.3333.3", "33....33", "23333332", "........"],
    "stone":    ["........", "........", "..333...", ".31123..", "3122223.", "3222223.", ".33333..", "........"],
    "coin":     ["..3333..", ".311113.", "31122113", "31211213", "31211213", "31122113", ".311113.", "..3333.."],
}
# Sprite palette 0 (OBP0 0xD0) shows colour 1 white, 2 light grey, 3 black: map the art's shades
# (1 light, 2 dark, 3 black in the browser) onto it as 1 -> 1, 2 -> 2, 3 -> 3.


def item_grid(name, w=8, h=16):
    g = [[0] * w for _ in range(h)]
    for y, row in enumerate(ITEM_ART[name]):
        for x, c in enumerate(row):
            if c != ".":
                g[y][x] = int(c)
    return g


def bush_art():
    """The browser's 24 x 24 bush (same random sequence): -1 = none, else colour."""
    a, r = [], 7
    def rnd2():
        nonlocal r
        r = (r * 1103515245 + 12345) & 0x7fffffff
        return r / 0x7fffffff
    for y in range(24):
        for x in range(24):
            dx, dy = x - 11.5, y - 12
            d = math.hypot(dx, dy * 1.08) + math.sin(math.atan2(dy, dx) * 7) * 1.2
            c = -1
            if d < 11.6:
                c = 2
                if d > 10.4:
                    c = 3
                elif rnd2() < 0.18:
                    c = 3
                elif dy < -2 and dx < 3 and rnd2() < 0.3:
                    c = 1
            a.append(c)
    return a


def bush_tiles():
    """Bush over plain ground, cut into cells for each placement offset used by the levels:
    (0, 0): 3 x 3 cells, (4, 0): 4 x 3, (0, 4): 3 x 4. Order: row by row."""
    art = bush_art()
    sets = []
    for ox, oy in ((0, 0), (4, 0), (0, 4)):
        cw, ch = (24 + ox + 7) // 8, (24 + oy + 7) // 8
        tiles = []
        for cy in range(ch):
            for cx in range(cw):
                g = [[0] * 8 for _ in range(8)]
                for y in range(8):
                    for x in range(8):
                        ax, ay = cx * 8 + x - ox, cy * 8 + y - oy
                        if 0 <= ax < 24 and 0 <= ay < 24 and art[ay * 24 + ax] >= 0:
                            g[y][x] = art[ay * 24 + ax]
                tiles.append(tile_bytes(g))
        sets.append(tiles)
    return sets


def icon_tiles():
    """Item icons for the item bar (light background, like the browser's slot)."""
    out = []
    for name in ITEM_ORDER:
        g = [[0] * 8 for _ in range(8)]
        for y, row in enumerate(ITEM_ART[name]):
            for x, c in enumerate(row):
                if c != ".":
                    g[y][x] = int(c)
        out.append(tile_bytes(g))
    return out


def extra_sprites(hero_img):
    """Stage 3 sprites (8x16 pairs): items, shadow, aiming cross, coin glint (4 sizes),
    see-through silhouettes (crouching player, lying body; dithered, light)."""
    spr = []
    for name in ITEM_ORDER:
        spr += sprite8x16(item_grid(name))
    g = [[0] * 8 for _ in range(16)]                       # shadow: 4 x 1 dark, at row 0
    for x in range(2, 6):
        g[0][x] = 2
    spr += sprite8x16(g)
    g = [[0] * 8 for _ in range(16)]                       # cross centred at (4, 4)
    for x in (0, 1, 2, 6, 7):
        g[4][x] = 3
    for y in (0, 1, 2, 6, 7):
        g[y][4] = 3
    spr += sprite8x16(g)
    for n in (1, 2, 3, 4):                                  # glint: star centred at (4, 4)
        g = [[0] * 8 for _ in range(16)]
        g[4][4] = 1
        for k in range(1, n + 1):
            for dx, dy in ((-1, 0), (1, 0), (0, -1), (0, 1)):
                x, y = 4 + dx * k, 4 + dy * k
                if 0 <= x < 8 and 0 <= y < 8:
                    g[y][x] = 3 if (k == n and n > 1) else 1
        spr += sprite8x16(g)
    down = hero_cell(hero_img, 0, 0)                        # crouching: waist rows 7-9 left out
    crouch = [[0] * 16 for _ in range(16)]
    for sy in range(16):
        if 7 <= sy <= 9:
            continue
        ty = sy + 3 if sy < 7 else sy
        for x in range(16):
            crouch[ty][x] = down[sy][x]
    side = hero_cell(hero_img, 6, 0)
    lying = [[side[x][15 - y] for x in range(16)] for y in range(16)]
    # every other pixel, flat: crouching player white, lying body light grey (palette 0), so they
    # read against the dark leaves
    for fig, c in ((crouch, 1), (lying, 2)):
        g = [[(c if fig[y][x] and not ((x + y) & 1) else 0) for x in range(16)] for y in range(16)]
        spr += sprite16(g)
    return spr


def cone_quads():
    """Per angle: the 4 x 4 px quarter-cells ("quads") of the cone, as (dx, dy) quad offsets from
    the eye's quad: a quad is in when its centre is within CONE_LEN of the eye's quad centre and
    within CONE_DEG / 2 of the facing. Format per angle: count, then dx, dy (signed)."""
    half = CONE_DEG / 2
    data, offs, tot = [], [], 0
    R = CONE_LEN // 4 + 1
    for k in range(CONE_ANGLES):
        th = 2 * math.pi * k / CONE_ANGLES
        qs = []
        for dy in range(-R, R + 1):
            for dx in range(-R, R + 1):
                if dx == 0 and dy == 0:
                    continue
                x, y = dx * 4, dy * 4
                if math.hypot(x, y) > CONE_LEN + 0.5:
                    continue
                a = math.degrees(math.atan2(y, x) - th)
                a = (a + 180) % 360 - 180
                if abs(a) > half + 0.01:
                    continue
                qs.append((dx, dy))
        qs.sort(key=lambda q: (q[1], q[0]))
        offs.append(len(data))
        data.append(len(qs))
        for dx, dy in qs:
            data += [dx & 0xFF, dy & 0xFF]
        tot += len(qs)
    return data, offs, tot // CONE_ANGLES


def lit_tiles(base_tiles):
    """For each base tile that cones can light (ground and floor): 15 variants, one per
    combination of lit quarters (bit 1 top left, 2 top right, 4 bottom left, 8 bottom right).
    Lit = checkerboard dots in colour 1 on the tile's colour-0 pixels."""
    out = []
    for t in base_tiles:
        px = [[0] * 8 for _ in range(8)]
        for y in range(8):
            lo, hi = t[2 * y], t[2 * y + 1]
            for x in range(8):
                px[y][x] = ((lo >> (7 - x)) & 1) | (((hi >> (7 - x)) & 1) << 1)
        for m in range(1, 16):
            g = [row[:] for row in px]
            for y in range(8):
                for x in range(8):
                    q = (1 if x >= 4 else 0) | (2 if y >= 4 else 0)
                    if (m >> q) & 1 and not ((x + y) & 1) and g[y][x] == 0:
                        g[y][x] = 1
            out.append(tile_bytes(g))
    return out


def cone_rows():
    """Per angle: the cone as one span [x0, x1] per pixel row (a sector under 180 degrees is
    convex, so every row is a single span). Format: dy0, rows, then x0, x1 per row (signed)."""
    half = CONE_DEG / 2
    data, offs, px = [], [], 0
    for k in range(CONE_ANGLES):
        th = 2 * math.pi * k / CONE_ANGLES
        rows = {}
        for dy in range(-CONE_LEN, CONE_LEN + 1):
            for dx in range(-CONE_LEN, CONE_LEN + 1):
                d = math.hypot(dx, dy)
                if d < 1 or d > CONE_LEN + 0.25:
                    continue
                a = math.degrees(math.atan2(dy, dx) - th)
                a = (a + 180) % 360 - 180
                if abs(a) > half + 0.01:
                    continue
                rows.setdefault(dy, []).append(dx)
        dys = sorted(rows)
        assert dys == list(range(dys[0], dys[-1] + 1))
        offs.append(len(data))
        data += [dys[0] & 0xFF, len(dys)]
        for dy in dys:
            xs = sorted(rows[dy])
            assert xs == list(range(xs[0], xs[-1] + 1)), (k, dy, xs)
            data += [xs[0] & 0xFF, xs[-1] & 0xFF]
            px += len(xs)
    return data, offs, px // CONE_ANGLES


def cell_los():
    """Cells strictly between cell (0,0) and cell (dx,dy), for one octant 0 <= dy <= dx <= LOS_R
    (the game mirrors the others): a cell is visible from the eye's cell if none of these is a
    wall. Index dx*(dx+1)/2 + dy; per entry: count, then one byte per cell, dx << 4 | dy."""
    data, offs = [], []
    for dx in range(LOS_R + 1):
        for dy in range(dx + 1):
            cells = []
            n = 64
            for i in range(1, n):
                t = i / n
                # the line between the cell centres, nudged off exact corners
                fx, fy = 0.5 + dx * t + 1e-4, 0.5 + dy * t + 2e-4
                c = (math.floor(fx), math.floor(fy))
                if c not in ((0, 0), (dx, dy)) and c not in cells:
                    cells.append(c)
            offs.append(len(data))
            data.append(len(cells))
            data += [(cx << 4) | cy for cx, cy in cells]
    return data, offs


def c_array(name, data, per=16, typ="uint8_t"):
    lines = [f"const {typ} {name}[{len(data)}] = {{"]
    for i in range(0, len(data), per):
        lines.append("    " + ", ".join((f"0x{b:02X}" if typ == "uint8_t" else str(b)) for b in data[i:i + per]) + ",")
    lines.append("};")
    return "\n".join(lines)


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    img = Image.open(sys.argv[1]).convert("RGB")

    frames, anim = [], []
    for row in range(8):
        steps = []
        for col in range(4):
            data = sprite16(hero_cell(img, row, col))
            if data not in frames:
                frames.append(data)
            steps.append(frames.index(data))
        anim.append(steps)
    assert len(frames) == 24, len(frames)
    spr = []
    for f in frames:
        spr += f
    side = hero_cell(img, 6, 0)
    lying = [[side[x][15 - y] for x in range(16)] for y in range(16)]
    spr += sprite16(lying)
    spr += sprite8x16(grid_from_art(ART_QUESTION, 8, 16))
    spr += sprite8x16(grid_from_art(ART_BANG, 8, 16))
    for f in exit_sprites():
        spr += f
    for b in bar_sprites():
        spr += b
    nspr = len(spr) // 16

    bg = []
    for t in ground_tiles() + floor_tiles():
        bg += t
    bg += wall_tile()
    for ch in FONT_CHARS:
        bg += glyph(ch)
    for t in arrow_tiles():
        bg += t
    for t in pit_tiles():
        bg += t
    bush_sets = bush_tiles()
    for st in bush_sets:
        for t in st:
            bg += t
    for t in icon_tiles():
        bg += t
    nbg = len(bg) // 16
    xspr = extra_sprites(img)
    nxspr = len(xspr) // 16
    lit_src = ground_tiles() + floor_tiles()          # base tiles 0..4 can be lit
    lit = []
    for t in lit_tiles(lit_src):
        lit += t
    nlit = len(lit) // 16

    cone, offs, avg = cone_quads()
    los, los_offs = cell_los()

    credit = ("/* Generated by tools/make_gfx.py - do not edit.\n"
              " * Hero art: \"8-Directional Game Boy Character Template\" by GibbonGL (CC0 1.0),\n"
              " * https://gibbongl.itch.io/8-directional-gameboy-character-template\n"
              " * All other graphics drawn for this game in tools/make_gfx.py. */\n")
    out_c = sys.argv[2]                                      # src/gfx.c (bank 0); also gfx_tiles.c, gfx_cone.c
    out_dir = out_c.rsplit("/", 1)[0] + "/" if "/" in out_c else ""
    with open(out_c, "w") as f:
        f.write(credit + '#include <stdint.h>\n#include "gfx.h"\n\n')
        f.write("const uint8_t hero_anim[8][4] = {\n")
        for steps in anim:
            f.write("    { " + ", ".join(str(s) for s in steps) + " },\n")
        f.write("};\n")
    with open(out_dir + "gfx_tiles.c", "w") as f:            # bank 2: loaded into VRAM once
        f.write(credit + '#pragma bank 2\n#include <gb/gb.h>\n#include <stdint.h>\n#include "gfx.h"\n\n')
        f.write(c_array("spr_tiles", spr) + "\n\n" + c_array("bg_tiles", bg) + "\n\n")
        f.write("/* lit tiles (loaded at B_LIT): 15 per base tile 0..4 */\n" + c_array("lit_tiles", lit) + "\n\n")
        f.write("/* stage 3 sprites (loaded at T_ITEM) */\n" + c_array("xspr_tiles", xspr) + "\n\n")
        f.write("""/* Load all tiles into VRAM (LCD off). Lives in bank 2 with the data. */
void load_tiles(void) BANKED
{
    set_sprite_data(0, SPR_TILES, spr_tiles);
    set_sprite_data(B_LIT, LIT_TILES, lit_tiles);   /* (background tiles >= 128 share this memory) */
    set_sprite_data(T_ITEM, XSPR_TILES, xspr_tiles);
    set_bkg_data(0, BG_TILES, bg_tiles);
}
""")
    with open(out_dir + "gfx_cone.c", "w") as f:             # bank 1: with the cone code
        f.write(credit + '#pragma bank 1\n#include <stdint.h>\n#include "gfx.h"\n\n')
        f.write(f"/* view cones: {CONE_LEN} px, {CONE_DEG} degrees, {CONE_ANGLES} angles, ~{avg} quads (4 x 4 px) each */\n")
        f.write(c_array("cone_off", offs, 8, "uint16_t") + "\n\n")
        f.write(c_array("cone_data", cone) + "\n\n")
        f.write("/* cells between two cells, for line of sight (one octant, index dx*(dx+1)/2 + dy) */\n")
        f.write(c_array("los_off", los_offs, 8, "uint16_t") + "\n\n")
        f.write(c_array("los_data", los) + "\n")
    with open(sys.argv[3], "w") as f:
        f.write(credit + f"""#ifndef GFX_H
#define GFX_H
#include <stdint.h>
#include <gb/gb.h>

/* gfx_tiles.c (bank 2): tile data, loaded once */
void load_tiles(void) BANKED;
/* gfx_cone.c (bank 1): cone and line-of-sight tables, only for cone.c (also bank 1) */

extern const uint8_t spr_tiles[{nspr} * 16];
extern const uint8_t bg_tiles[{nbg} * 16];
extern const uint8_t hero_anim[8][4];   /* [sheet row][walk step] -> frame (tiles frame*4) */
extern const uint16_t cone_off[{CONE_ANGLES}];
extern const uint8_t cone_data[{len(cone)}];   /* per angle: count, then (dx, dy) quad offsets */
extern const uint8_t lit_tiles[{nlit} * 16];
extern const uint16_t los_off[{(LOS_R + 1) * (LOS_R + 2) // 2}];
extern const uint8_t los_data[{len(los)}];

#define SPR_TILES   {nspr}u
#define BG_TILES    {nbg}u
#define LIT_TILES   {nlit}u
#define LIT_BASES   5u      /* base tiles 0..LIT_BASES-1 can be lit */
#define XSPR_TILES  {nxspr}u
#define CONE_LEN    {CONE_LEN}u
#define CONE_ANGLES {CONE_ANGLES}u
#define LOS_R       {LOS_R}u
#define LOS_W       {2 * LOS_R + 1}u

/* sprite tiles */
#define T_LYING     96u
#define T_QUESTION  100u
#define T_BANG      102u
#define T_EXIT      104u    /* 3 frames x 4 tiles: open (2 drift steps), locked */
#define T_BAR       116u    /* 7 fills x 2 tiles */

/* background tiles */
#define B_GROUND    0u      /* 0..2 */
#define B_FLOOR     3u      /* 3..4 */
#define B_WALL      5u
#define B_FONT      6u      /* " A-Z0-9!,.-:" */
#define B_ARROW     (B_FONT + {len(FONT_CHARS)}u)   /* left, right */
#define B_PIT       (B_ARROW + 2u)  /* 16: ravine, by open neighbours up 1 left 2 right 4 down 8 */
#define B_LIT       (SPR_TILES > 128u ? SPR_TILES : 128u)   /* lit tiles: B_LIT + base * 15 + mask - 1 */
#define B_BUSH      (B_PIT + 16u)   /* bush cells: offset (0,0) 3x3, (4,0) 4x3, (0,4) 3x4 */
#define B_BUSH_X    (B_BUSH + 9u)
#define B_BUSH_Y    (B_BUSH_X + 12u)
#define B_ICON      (B_BUSH_Y + 12u)  /* item icons for the bar: shuriken rope trap stone coin */

/* stage 3 sprites (8x16 pairs, after the lit tiles) */
#define T_ITEM      ((B_LIT + LIT_TILES + 1u) & 0xFEu)   /* 5 items x 2 */
#define T_SHADOW    (T_ITEM + 10u)
#define T_CROSS     (T_ITEM + 12u)
#define T_GLARE     (T_ITEM + 14u)  /* 4 sizes x 2 */
#define T_GHOST     (T_ITEM + 22u)  /* see-through: crouching (4 tiles), lying (4 tiles) */

#endif
""")
    print(f"gfx: {nspr} sprite tiles, {nbg} background tiles, cone tables {len(cone)} bytes (~{avg} px per cone)")


if __name__ == "__main__":
    main()
