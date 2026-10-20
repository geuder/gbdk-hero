/*
 * cone.c - NPC view cones, drawn by swapping background tiles.
 *
 * The play area is split into 4 x 4 px quarter-cells ("quads"), 40 x 34 of them. A cone is the
 * set of quads whose centre lies within 60 px and 15 degrees of the NPC's facing (a table per
 * facing angle, tools/make_gfx.py), and that the NPC can see: walls block the view per cell.
 * qbits[] keeps, per quad, one bit per cone lighting it. A cell shows the lit variant of its
 * tile for the quads that are lit (16 combinations, made by the generator: checkerboard dots
 * on the tile's light pixels), so moving a cone only rewrites map entries, one byte per cell.
 *
 * Detection reads the same bits: an NPC sees the player when a quad under the player's body
 * carries its bit, so what's on screen is exactly what the NPC sees.
 *
 * A cone is moved by clearing its old quads and setting its new ones, a few quads at a time
 * within the frame's time budget; cones take turns.
 */
#include <gb/gb.h>
#include <stdint.h>
#include <string.h>
#include "game.h"
#include "gfx.h"

#define QDW 40u                         /* quads per row */
#define QH 34u
#define VC_BYTES 73u                    /* 17 x 17 cells, 2 bits each */

uint8_t qbits[QDW * QH];                 /* per quad: bit per cone */
static uint8_t cshown[CELLS_W * CELLS_H];   /* lit quarters shown per cell */
uint8_t los_open;                       /* the level has no walls: every cell is visible */

static uint8_t vc[NCONES][VC_BYTES];    /* per cone: visibility of the cells around its eye cell */
static uint16_t vc_eye[NCONES];

/* per cone: what is drawn now, and what it should become */
static uint8_t s_on[NCONES], s_qx[NCONES], s_qy[NCONES], s_k[NCONES], s_ex[NCONES], s_ey[NCONES];
static uint8_t t_on[NCONES], t_qx[NCONES], t_qy[NCONES], t_k[NCONES], t_ex[NCONES], t_ey[NCONES];

/* the job in progress: cone jn */
static uint8_t jn = 0xFF;
uint8_t jbit;
static uint8_t next_cone;
uint8_t cr_lylimit = 255;

static const uint16_t QROW[QH] = {
    0, 40, 80, 120, 160, 200, 240, 280, 320, 360, 400, 440, 480, 520, 560, 600, 640, 680,
    720, 760, 800, 840, 880, 920, 960, 1000, 1040, 1080, 1120, 1160, 1200, 1240, 1280, 1320 };

/* Frame position: scan lines since vblank started (0..153), so a frame's work doesn't wrap. */
static uint8_t frame_pos(void)
{
    uint8_t l = LY_REG;
    return l >= 144u ? (uint8_t)(l - 144u) : (uint8_t)(l + 10u);
}
#define OUT_OF_TIME() ((LCDC_REG & LCDCF_ON) && frame_pos() > cr_lylimit)

void cone_reset(void)
{
    memset(qbits, 0, sizeof qbits);
    memset(cshown, 0, sizeof cshown);
    memset(s_on, 0, sizeof s_on);
    memset(t_on, 0, sizeof t_on);
    memset(vc_eye, 0xFF, sizeof vc_eye);
    jn = 0xFF; next_cone = 0;
}

/* ---- line of sight, per cell, cached per cone for its eye cell ---- */
static uint8_t v_ecx, v_ecy;
static uint16_t v_ec;
static uint8_t *v_cache;

static const int16_t ROW22[17] = {            /* dy * PW, dy = -8..8 */
    -176, -154, -132, -110, -88, -66, -44, -22, 0, 22, 44, 66, 88, 110, 132, 154, 176 };
static const uint8_t W17[17] = { 0, 17, 34, 51, 68, 85, 102, 119, 136, 153, 170, 187, 204, 221, 238, 255, 255 };

static uint8_t cell_vis(uint8_t cx, uint8_t cy)       /* 1 = visible, 2 = hidden */
{
    int8_t dx = (int8_t)(cx - v_ecx), dy = (int8_t)(cy - v_ecy);
    uint8_t ax, ay, sw, n, b, a, c;
    int8_t ox, oy;
    const uint8_t *p;
    if (solid[PIDX(cx, cy)] & OPAQUE) return 2;        /* the cell itself is a wall */
    ax = (uint8_t)(dx < 0 ? -dx : dx); ay = (uint8_t)(dy < 0 ? -dy : dy);
    sw = ay > ax;                                      /* mirror into the octant 0 <= y <= x */
    if (sw) { a = ax; ax = ay; ay = a; }
    p = &los_data[los_off[(uint8_t)((ax * (ax + 1u)) >> 1) + ay]];
    for (n = *p++; n; n--) {
        b = *p++;
        a = b >> 4; c = b & 15u;
        if (sw) { ox = (int8_t)c; oy = (int8_t)a; } else { ox = (int8_t)a; oy = (int8_t)c; }
        if (dx < 0) ox = -ox;
        if (dy < 0) oy = -oy;
        if (solid[(uint16_t)((int16_t)v_ec + ox + ROW22[(uint8_t)(oy + 8)])] & OPAQUE) return 2;
    }
    return 1;
}

static void vis_setup(uint8_t n, uint8_t ex, uint8_t ey)
{
    v_ecx = ex >> 3; v_ecy = ey >> 3;
    v_ec = ROWP[v_ecy] + v_ecx;
    v_cache = vc[n];
    if (vc_eye[n] != v_ec) { vc_eye[n] = v_ec; memset(v_cache, 0, VC_BYTES); }
}

static uint8_t vis(uint8_t cx, uint8_t cy)             /* cached */
{
    uint8_t wx = (uint8_t)(cx - v_ecx + LOS_R), wy = (uint8_t)(cy - v_ecy + LOS_R), sh, v;
    uint16_t i;
    uint8_t *b;
    if (wx >= LOS_W || wy >= LOS_W) return 2;
    i = (uint16_t)(wy < 15u ? W17[wy] : (uint16_t)wy * LOS_W) + wx;
    b = &v_cache[i >> 2]; sh = (uint8_t)((i & 3u) << 1);
    v = (uint8_t)((*b >> sh) & 3u);
    if (!v) { v = cell_vis(cx, cy); *b |= (uint8_t)(v << sh); }
    return v;
}

/* ---- moving a cone (assembly) ----
 * The old and new quad lists are both sorted by (y, x), so they are merged: quads only in the
 * old list are cleared, quads only in the new one are set (where the NPC can see), quads in both
 * stay (on levels with walls they are looked at again when the eye moved to another cell). Only
 * the quads that change are touched, so a cone never disappears while it moves. When a quad
 * changed, the cell's lit quarters are worked out again and, if they differ from what's shown,
 * its map entry is queued. Stops when out of time (returns 0) or done (1). */
static const uint16_t CROW[CELLS_H] = {         /* cell row * 20 */
    0, 20, 40, 60, 80, 100, 120, 140, 160, 180, 200, 220, 240, 260, 280, 300, 320 };
static const uint8_t LITB[LIT_BASES] = {        /* lit tile for base b, mask m: LITB[b] + m */
    B_LIT - 1u, B_LIT + 14u, B_LIT + 29u, B_LIT + 44u, B_LIT + 59u };
uint8_t jtile, jrecheck, jtick;
/* old list: pointer, count, origin (+32); new list: the same */
const uint8_t *ja_p, *jb_p;
uint8_t ja_n, jb_n, ja_x, ja_y, jb_x, jb_y;

uint8_t vis_c(uint8_t cx, uint8_t cy);          /* line of sight for the cell: 1 = visible */

static uint8_t job_run(void) __naked
{
    __asm
1$:
    ld a, (_ja_n)               ; both lists done?
    ld b, a
    ld a, (_jb_n)
    or a, b
    jr nz, 2$
    ld a, #1
    ret
2$:
    ld a, (_los_open)           ; out of time? queue full? (every quad with walls, else every 4)
    or a, a
    jr z, 20$
    ld hl, #_jtick
    inc (hl)
    ld a, (hl)
    and a, #3
    jr nz, 4$
20$:
    ldh a, (#0x40)
    bit 7, a
    jr z, 3$
    ldh a, (#0x44)              ; frame position = lines since vblank started
    add a, #10
    cp a, #154
    jr c, 21$
    sub a, #154
21$:
    ld b, a
    ld a, (_cr_lylimit)
    cp a, b
    jr nc, 3$
    xor a, a                    ; out of time
    ret
3$:
    ld a, (_qp+1)               ; queue: room for a few more?
    cp a, #>(_q + 3 * (320 - 8))
    jr c, 4$
    jr nz, 31$
    ld a, (_qp)
    cp a, #<(_q + 3 * (320 - 8))
    jr c, 4$
31$:
    xor a, a
    ret
4$:                             ; ---- which list is next? keys (y, x), +32
    ld a, (_ja_n)
    or a, a
    jp z, 60$                   ; old list done: new
    ld a, (_jb_n)
    or a, a
    jr z, 50$                   ; new list done: old
    ld a, (_ja_p)               ; D = old y, E = old x
    ld l, a
    ld a, (_ja_p+1)
    ld h, a
    ld a, (_ja_x)
    add a, (hl)
    ld e, a
    inc hl
    ld a, (_ja_y)
    add a, (hl)
    ld d, a
    ld a, (_jb_p)               ; B = new y, C = new x
    ld l, a
    ld a, (_jb_p+1)
    ld h, a
    ld a, (_jb_x)
    add a, (hl)
    ld c, a
    inc hl
    ld a, (_jb_y)
    add a, (hl)
    ld b, a
    ld a, d                     ; compare y, then x
    cp a, b
    jr c, 50$                   ; old first
    jr nz, 60$                  ; new first
    ld a, e
    cp a, c
    jr c, 50$
    jr nz, 60$
                                ; ---- in both: next of each (and with walls, look again if the eye moved)
    ld hl, #_ja_n
    dec (hl)
    ld hl, #_jb_n
    dec (hl)
    ld hl, #_ja_p
    call 90$
    ld hl, #_jb_p
    call 90$
    ld a, (_jrecheck)
    or a, a
    jp z, 1$
    ld a, b                     ; B = x, C = y (play area)
    sub a, #32
    ld b, a
    ld a, c
    sub a, #32
    ld c, a
    ld a, b
    ld d, c                     ; (swap: B = x, C = y)
    ld b, c
    ld c, a
    ld a, d
    ld b, a
    jp 70$
50$:                            ; ---- only in the old list: clear
    ld a, (_ja_p)
    ld l, a
    ld a, (_ja_p+1)
    ld h, a
    ld a, (_ja_x)
    add a, (hl)
    sub a, #32
    ld b, a                     ; B = x
    inc hl
    ld a, (_ja_y)
    add a, (hl)
    sub a, #32
    ld c, a                     ; C = y
    ld hl, #_ja_n
    dec (hl)
    ld hl, #_ja_p
    call 90$
    call 80$                    ; HL = quad, or NC: off the play area
    jp nc, 1$
    ld a, (_jbit)
    and a, (hl)
    jp z, 1$
    ld a, (_jbit)
    cpl
    and a, (hl)
    ld (hl), a
    jp 100$
60$:                            ; ---- only in the new list: set, where the NPC can see
    ld a, (_jb_p)
    ld l, a
    ld a, (_jb_p+1)
    ld h, a
    ld a, (_jb_x)
    add a, (hl)
    sub a, #32
    ld b, a
    inc hl
    ld a, (_jb_y)
    add a, (hl)
    sub a, #32
    ld c, a
    ld hl, #_jb_n
    dec (hl)
    ld hl, #_jb_p
    call 90$
    call 80$
    jp nc, 1$
    ld a, (_jbit)
    and a, (hl)
    jp nz, 1$
    call 85$                    ; visible?
    jp nz, 1$
    ld a, (_jbit)
    or a, (hl)
    ld (hl), a
    jp 100$
70$:                            ; ---- look again (B = x, C = y): set or clear by visibility
    call 80$
    jp nc, 1$
    call 85$
    jr nz, 71$
    ld a, (_jbit)               ; visible: set
    and a, (hl)
    jp nz, 1$
    ld a, (_jbit)
    or a, (hl)
    ld (hl), a
    jp 100$
71$:
    ld a, (_jbit)               ; hidden: clear
    and a, (hl)
    jp z, 1$
    ld a, (_jbit)
    cpl
    and a, (hl)
    ld (hl), a
    jp 100$

80$:                            ; ---- B = x, C = y -> HL = quad, carry set if on the play area
    ld a, b
    cp a, #40
    ret nc
    ld a, c
    cp a, #34
    ret nc
    ld l, c
    ld h, #0
    add hl, hl
    ld de, #_QROW
    add hl, de
    ld a, (hl+)
    ld h, (hl)
    ld l, a
    ld e, b
    ld d, #0
    add hl, de
    ld de, #_qbits
    add hl, de
    scf
    ret

85$:                            ; ---- can the NPC see quad (B, C)? Z = yes (HL, BC kept)
    ld a, (_los_open)
    or a, a
    jr z, 86$
    xor a, a
    ret
86$:
    push hl
    push bc
    ld a, b
    srl a
    ld e, c
    srl e
    call _vis_c
    pop bc
    pop hl
    dec a
    ret

90$:                            ; ---- (HL) += 2: next entry of a list
    ld a, (hl)
    add a, #2
    ld (hl+), a
    ret nc
    inc (hl)
    ret

100$:                           ; ---- cell (x / 2, y / 2): lit quarters
    srl b                       ; B = cx
    srl c                       ; C = cy
    ld l, c                     ; base tile = base_tile[ROWP[cy] + cx]
    ld h, #0
    add hl, hl
    ld de, #_ROWP
    add hl, de
    ld a, (hl+)
    ld h, (hl)
    ld l, a
    ld e, b
    ld d, #0
    add hl, de
    ld de, #_base_tile
    add hl, de
    ld a, (hl)
    cp a, #5                    ; LIT_BASES: walls, ravine never lit
    jp nc, 1$
    ld (_jtile), a
    ld l, c                     ; HL = qbits + QROW[2 cy] + 2 cx
    ld h, #0
    add hl, hl
    add hl, hl
    ld de, #_QROW
    add hl, de
    ld a, (hl+)
    ld h, (hl)
    ld l, a
    ld e, b
    sla e
    ld d, #0
    add hl, de
    ld de, #_qbits
    add hl, de
    ld d, #0                    ; D = mask
    ld a, (hl+)
    or a, a
    jr z, 101$
    set 0, d
101$:
    ld a, (hl)
    or a, a
    jr z, 102$
    set 1, d
102$:
    ld a, l
    add a, #39
    ld l, a
    jr nc, 103$
    inc h
103$:
    ld a, (hl+)
    or a, a
    jr z, 104$
    set 2, d
104$:
    ld a, (hl)
    or a, a
    jr z, 105$
    set 3, d
105$:
    ld l, c                     ; HL = cshown + CROW[cy] + cx
    ld h, #0
    add hl, hl
    ld a, l
    add a, #<(_CROW)
    ld l, a
    ld a, h
    adc a, #>(_CROW)
    ld h, a
    ld a, (hl+)
    ld h, (hl)
    ld l, a
    ld a, l
    add a, b
    ld l, a
    jr nc, 106$
    inc h
106$:
    ld a, l
    add a, #<(_cshown)
    ld l, a
    ld a, h
    adc a, #>(_cshown)
    ld h, a
    ld a, (hl)
    cp a, d
    jp z, 1$                    ; unchanged
    ld (hl), d
    ld a, d                     ; tile = mask ? LITB[base] + mask : base
    or a, a
    ld a, (_jtile)
    jr z, 107$
    ld e, a
    ld a, #<(_LITB)
    add a, e
    ld l, a
    ld a, #>(_LITB)
    adc a, #0
    ld h, a
    ld a, (hl)
    add a, d
107$:
    ld e, a                     ; E = tile
    ld a, (_qp)                 ; queue (0x9800 + cy * 32 + cx, tile)
    ld l, a
    ld a, (_qp+1)
    ld h, a
    ld a, c
    and a, #7
    swap a
    add a, a                    ; (cy & 7) << 5
    add a, b
    ld (hl+), a
    ld a, c
    srl a
    srl a
    srl a
    add a, #0x98
    ld (hl+), a
    ld (hl), e
    inc hl
    ld a, l
    ld (_qp), a
    ld a, h
    ld (_qp+1), a
    jp 1$
    __endasm;
}

uint8_t vis_c(uint8_t cx, uint8_t cy) { return vis(cx, cy); }

/* Start moving cone n from what's drawn (s_) to the target (t_). */
static void job_start(uint8_t n)
{
    const uint8_t *p;
    jbit = (uint8_t)(1u << n);
    if (s_on[n]) { p = &cone_data[cone_off[s_k[n]]]; ja_n = p[0]; ja_p = p + 1; ja_x = (uint8_t)(s_qx[n] + 32u); ja_y = (uint8_t)(s_qy[n] + 32u); }
    else ja_n = 0;
    if (t_on[n]) {
        p = &cone_data[cone_off[t_k[n]]]; jb_n = p[0]; jb_p = p + 1; jb_x = (uint8_t)(t_qx[n] + 32u); jb_y = (uint8_t)(t_qy[n] + 32u);
        jrecheck = !los_open && s_on[n] && ((s_ex[n] >> 3) != (t_ex[n] >> 3) || (s_ey[n] >> 3) != (t_ey[n] >> 3));
        vis_setup(n, t_ex[n], t_ey[n]);
    } else { jb_n = 0; jrecheck = 0; }
    s_on[n] = t_on[n]; s_qx[n] = t_qx[n]; s_qy[n] = t_qy[n]; s_k[n] = t_k[n]; s_ex[n] = t_ex[n]; s_ey[n] = t_ey[n];
}

/* Call once per frame with every cone's eye and facing; stop at frame position lylimit. */
void cones_update(const uint8_t *on, const uint8_t *ex, const uint8_t *ey, const uint8_t *k, uint8_t lylimit)
{
    uint8_t n, i, f;
    for (n = 0; n < NCONES; n++) {                     /* targets */
        t_on[n] = on[n];
        if (on[n]) { t_ex[n] = ex[n]; t_ey[n] = ey[n]; t_qx[n] = ex[n] >> 2; t_qy[n] = ey[n] >> 2; t_k[n] = k[n]; }
    }
    cr_lylimit = lylimit;
    f = frame_pos();
    if ((LCDC_REG & LCDCF_ON) && f > lylimit) return;   /* no time left this frame */
    while (1) {
        if (jn == 0xFFu) {                             /* next cone that changed, in turn */
            for (i = 0; i < NCONES; i++) {
                n = next_cone;
                if (++next_cone >= NCONES) next_cone = 0;
                if (t_on[n] != s_on[n] || (t_on[n] && (t_qx[n] != s_qx[n] || t_qy[n] != s_qy[n] || t_k[n] != s_k[n]
                    || (!los_open && ((t_ex[n] >> 3) != (s_ex[n] >> 3) || (t_ey[n] >> 3) != (s_ey[n] >> 3)))))) break;
            }
            if (i == NCONES) return;                   /* all up to date */
            jn = n;
            job_start(n);
        }
        if (!job_run()) return;                        /* out of time: next frame */
        jn = 0xFF;
        if (OUT_OF_TIME()) return;
    }
}

/* Does cone n light any quad of the w x h box at (bx, by)? */
uint8_t cone_sees_box(uint8_t n, uint8_t bx, uint8_t by, uint8_t w, uint8_t h)
{
    uint8_t x0 = bx >> 2, x1 = (uint8_t)(bx + w - 1u) >> 2, y0 = by >> 2, y1 = (uint8_t)(by + h - 1u) >> 2, x, b = (uint8_t)(1u << n);
    const uint8_t *qb;
    if (bx >= SCR_W) x0 = 0;                           /* (box partly off the left / top edge) */
    if (x1 >= QDW) x1 = QDW - 1u;
    if (by >= PLAY_H) y0 = 0;
    if (y1 >= QH) y1 = QH - 1u;
    for (; y0 <= y1; y0++) {
        qb = &qbits[QROW[y0] + x0];
        for (x = x0; x <= x1; x++) if (*qb++ & b) return 1;
    }
    return 0;
}

uint8_t cone_sees(uint8_t n, uint8_t bx, uint8_t by) { return cone_sees_box(n, bx, by, 8u, 13u); }
