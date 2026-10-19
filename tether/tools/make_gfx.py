#!/usr/bin/env python3
"""
make_gfx.py - build all Game Boy tile data for Tether.

    python3 tools/make_gfx.py assets/hero.png src/gfx.c src/gfx.h

Everything except the hero is drawn here as ASCII art, so the whole game's
graphics live in this one file. Character key for the art:

    ' '  transparent (sprites) / background colour 0 (background tiles)
    '.'  colour 0 (lightest)     '+' colour 1     '=' colour 2     '#' colour 3

Hero: "8-Directional Game Boy Character Template" by GibbonGL (CC0 1.0),
https://gibbongl.itch.io/8-directional-gameboy-character-template
The sheet has no alpha channel; its dark green is the background and is
removed by flood fill from each 16x16 cell's border.

Output layout (keep in sync with src/gfx.h, which this script writes):

  Sprite tiles (VRAM 0x8000, indices 0..127, 8x16 sprite mode):
    0..95   hero: 24 unique frames x 4 tiles (left-top, left-bottom,
            right-top, right-bottom). hero_anim[dir][step] -> frame.
    96..99  hero lying on the sea floor (side frame turned 90 degrees)
    100..107 fish: frame 0 (tail open), frame 1 (tail closed), 4 tiles each
    108..113 bubbles: small, ring, big ring (top tile + blank bottom)
    114..121 gem: plain, sparkle 1, 2, 3
    122..123 hook        124..125 hook line      126..127 arrow

  Background/window tiles (BG index 0..89, VRAM 0x9000 region, LCDC bit 4 = 0):
    0..4 seabed (only colours 0 and 1, so the hose can be drawn in the
    high bitplane), 5..8 rock, 9..16 pump, 17..58 font, 59..89 HUD.
  Background indices 128..255 (VRAM 0x8800..0x8FFF) are the hose tile pool.
"""

import sys
from PIL import Image

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


# ---------------------------------------------------------------- art
ART_FISH = [                      # faces left, mouth at x = 0
    "   #####    ",
    " ##++++=#  #",
    "#+.#+++==#=#",
    "#++++++===##",
    "#=+++++===##",
    " #==+++==#=#",
    "  ##=====# #",
    "    ####    "]
ART_FISH_FLAP = [                 # tail fin closed
    "   #####    ",
    " ##++++=#   ",
    "#+.#+++==#  ",
    "#++++++===##",
    "#=+++++===##",
    " #==+++==#  ",
    "  ##=====#  ",
    "    ####    "]
ART_BUBBLE_SMALL = ["   ", "   ", "   ", "   =", ]
ART_BUBBLE_RING = ["", "  ==", " =  =", " =  =", "  =="]
ART_BUBBLE_BIG = ["  ==", " =  =", "=    =", "=    =", " =  =", "  =="]
ART_GEM = [
    "   ##   ",
    "  #..#  ",
    " #.++=# ",
    "#.++++=#",
    "#+++==+#",
    " #+===# ",
    "  #==#  ",
    "   ##   "]
ART_HOOK = [                      # line at x = 3 coming from above, J-hook at the bottom
    "   #",
    "   #",
    "   #",
    "   #",
    "   #",
    "   #  #",
    "    ##",
    ""]
ART_ARROW = [" #####", "  ###", "   #"]
ART_ROCK = [
    "                ",
    "     #####      ",
    "   ##+++++###   ",
    "  #++....++++#  ",
    " #++..+++++++=# ",
    " #+.++++++++++=#",
    "#++++++++==+++=#",
    "#+++++++====++=#",
    "#++++++++==+++=#",
    "#=++++++++++++=#",
    " #=++++++++++=# ",
    " #==++++++++==# ",
    "  ##=========#  ",
    "    #########   ",
    "                ",
    "                "]
ART_PUMP = [
    "  ############  ",
    " #============# ",
    "#=++++++++++++=#",
    "#=+..######..+=#",
    "#=+.#+====+#.+=#",
    "#=+#+=+..+=+#+=#",
    "#=+#=+.##.+=#+=#",
    "#=+#=.####.=#+=#",
    "#=+#=.####.=#+=#",
    "#=+#=+.##.+=#+=#",
    "#=+#+=+..+=+#+=#",
    "#=+.#+====+#.+=#",
    "#=+..######..+=#",
    "#=+++++LL+++++=#",
    "#==============#",
    " ############## "]

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


def ground_tiles():
    tiles = [tile_bytes([[0] * 8 for _ in range(8)])]
    ripple = [(0, 1), (1, 0), (2, 0), (3, 1), (4, 1), (5, 0)]
    for ox, oy in [(1, 2), (2, 5), (0, 4), (2, 1)]:
        g = [[0] * 8 for _ in range(8)]
        for a, b in ripple:
            if ox + a < 8:
                g[oy + b][ox + a] = 1
        tiles.append(tile_bytes(g))
    return tiles


def hud_tiles():
    t = [tile_bytes([[3] * 8 for _ in range(8)])]            # 59 blank
    for fill_col in (0, 1):                                  # 60..68, 69..77 air bar (6 px tall)
        for k in range(9):
            g = [[3] * 8 for _ in range(8)]
            for y in range(1, 7):
                for x in range(8):
                    g[y][x] = fill_col if x < k else 2
            t.append(tile_bytes(g))
    for k in range(9):                                       # 78..86 hose bar (2 px tall)
        g = [[3] * 8 for _ in range(8)]
        for y in (3, 4):
            for x in range(8):
                g[y][x] = 0 if x < k else 2
        t.append(tile_bytes(g))
    for col in (0, 2):                                       # 87 pip on, 88 pip off
        g = [[3] * 8 for _ in range(8)]
        for y in range(2, 6):
            for x in range(2, 5):
                g[y][x] = col
        t.append(tile_bytes(g))
    t.append(tile_bytes(grid_from_art([s.replace(".", "+") for s in ART_GEM], 8, 8)))  # 89 gem icon
    return t


def sparkle(n):
    g = grid_from_art(ART_GEM, 8, 8)
    cx, cy = 2, 2
    g[cy][cx] = 1
    for k in range(1, n + 1):
        col = 3 if k == n else 1
        for dx, dy in ((-k, 0), (k, 0), (0, -k), (0, k)):
            if 0 <= cx + dx < 8 and 0 <= cy + dy < 8:
                g[cy + dy][cx + dx] = col
    return g


def pad16(g8):
    return g8 + [[0] * len(g8[0]) for _ in range(16 - len(g8))]


def c_array(name, data, per=16):
    lines = [f"const uint8_t {name}[{len(data)}] = {{"]
    for i in range(0, len(data), per):
        lines.append("    " + ", ".join(f"0x{b:02X}" for b in data[i:i + per]) + ",")
    lines.append("};")
    return "\n".join(lines)


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    img = Image.open(sys.argv[1]).convert("RGB")

    # hero frames, deduplicated
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
    side = hero_cell(img, 6, 0)                               # facing right, standing
    lying = [[side[x][15 - y] for x in range(16)] for y in range(16)]   # turned 90 deg anticlockwise
    spr += sprite16(lying)
    for art in (ART_FISH, ART_FISH_FLAP):
        spr += sprite16(grid_from_art(art, 16, 16, 2, 0))
    for art in (ART_BUBBLE_SMALL, ART_BUBBLE_RING, ART_BUBBLE_BIG):
        spr += sprite8x16(grid_from_art(art, 8, 16, 0, 0))
    spr += sprite8x16(pad16(grid_from_art(ART_GEM, 8, 8)))
    for n in (1, 2, 3):
        spr += sprite8x16(pad16(sparkle(n)))
    spr += sprite8x16(grid_from_art(ART_HOOK, 8, 16))
    spr += sprite8x16([[3 if x == 3 else 0 for x in range(8)] for _ in range(16)])
    spr += sprite8x16(grid_from_art(ART_ARROW, 8, 16))
    assert len(spr) == 128 * 16, len(spr) // 16

    bg = []
    for t in ground_tiles():
        bg += t
    bg += bg16(grid_from_art(ART_ROCK, 16, 16))
    pump = {c: grid_from_art([r.replace("L", c) for r in ART_PUMP], 16, 16) for c in ("=", ".", "#")}
    bg += tile_bytes(pump["="], 0, 0) + tile_bytes(pump["="], 8, 0)          # 9, 10 top
    for c in ("=", ".", "#"):                                                # 11..16 bottoms: off, on, on2
        bg += tile_bytes(pump[c], 0, 8) + tile_bytes(pump[c], 8, 8)
    for ch in FONT_CHARS:
        bg += glyph(ch)
    for t in hud_tiles():
        bg += t
    assert len(bg) == 90 * 16, len(bg) // 16

    credit = ("/* Generated by tools/make_gfx.py - do not edit.\n"
              " * Hero art: \"8-Directional Game Boy Character Template\" by GibbonGL (CC0 1.0),\n"
              " * https://gibbongl.itch.io/8-directional-gameboy-character-template\n"
              " * All other graphics drawn for this game in tools/make_gfx.py. */\n")
    with open(sys.argv[2], "w") as f:
        f.write(credit + '#include <stdint.h>\n#include "gfx.h"\n\n')
        f.write(c_array("spr_tiles", spr) + "\n\n" + c_array("bg_tiles", bg) + "\n\n")
        f.write("const uint8_t hero_anim[8][4] = {\n")
        for steps in anim:
            f.write("    { " + ", ".join(str(s) for s in steps) + " },\n")
        f.write("};\n")
    with open(sys.argv[3], "w") as f:
        f.write(credit + """#ifndef GFX_H
#define GFX_H
#include <stdint.h>

extern const uint8_t spr_tiles[128 * 16];
extern const uint8_t bg_tiles[90 * 16];
extern const uint8_t hero_anim[8][4];   /* [direction][walk step] -> frame (tiles frame*4) */

/* sprite tiles */
#define T_LYING      96u
#define T_FISH       100u   /* + 4 for tail-closed frame */
#define T_BUB_SMALL  108u
#define T_BUB_RING   110u
#define T_BUB_BIG    112u
#define T_GEM        114u   /* + 2 per sparkle size */
#define T_HOOK       122u
#define T_LINE       124u
#define T_ARROW      126u

/* background / window tiles */
#define B_GROUND     0u     /* 0..4 */
#define B_ROCK       5u     /* TL TR BL BR */
#define B_PUMP_TOP   9u     /* TL TR */
#define B_PUMP_OFF   11u    /* BL BR */
#define B_PUMP_ON    13u
#define B_PUMP_ON2   15u
#define B_FONT       17u    /* " A-Z0-9!,.-:" */
#define B_HUD_BLANK  59u
#define B_AIR        60u    /* + fill 0..8 */
#define B_AIR_LOW    69u
#define B_HOSEBAR    78u
#define B_PIP_ON     87u
#define B_PIP_OFF    88u
#define B_GEM_ICON   89u
#define B_NUM_TILES  90u
#define B_POOL       128u   /* 128..255: hose tile pool */

#endif
""")
    print("gfx: 128 sprite tiles, 90 background tiles")


if __name__ == "__main__":
    main()
