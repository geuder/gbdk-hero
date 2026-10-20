/*
 * hose.c - drawing the air hose into background tiles.
 *
 * Sprites can't draw a long line (10 per scanline), so every 8x8 cell the
 * hose crosses gets its own tile from a pool of 128 (background indices
 * 128..255, VRAM 0x8800..0x8FFF). The pool tile is the cell's seabed tile
 * plus the hose pixels.
 *
 * Seabed tiles only use colours 0 and 1, so their high bitplane is empty.
 * The hose lives in the high bitplane only: colour 2 over sand, 3 over a
 * ripple pixel. After a pool tile is first written, updates only touch the
 * high-plane bytes of rows that changed.
 *
 * Two layers per tile: "fixed" (pump-to-rock segments, redrawn only when
 * the hose catches on or comes off a rock) and "live" (the segment to the
 * diver plus hole knots, redrawn whenever he moves). Only tiles touched by
 * the live layer this frame or last frame are compared and uploaded.
 *
 * Rock and pump cells are never drawn into: they cover the hose.
 */
#include <gb/gb.h>
#include <string.h>
#include "game.h"
#include "gfx.h"

#define POOL 128u
#define NONE 0xFFu

uint8_t base_tile[NCELLS];

static uint8_t  cell_slot[NCELLS];
static uint16_t slot_cell[POOL];
static uint8_t *slot_map[POOL];              /* BG map address of the slot's cell */
static uint8_t  active_pos[POOL];
/* Per pool tile, 8 rows of 3 bytes: fixed layer, live layer, what VRAM shows. */
static uint8_t  lay3[POOL][24];
static uint8_t *slot_lay[POOL];              /* = lay3[s], precomputed (no multiply) */
static uint8_t *slot_vram[POOL];             /* high-plane byte of row 0 of pool tile s */
static const uint8_t ROW3[8] = { 0, 3, 6, 9, 12, 15, 18, 21 };
static uint8_t  slot_new[POOL];
uint8_t  active[POOL], n_active;
static uint8_t  free_s[POOL], n_free;
static uint8_t  dirty[POOL];                 /* stamp: == stamp -> already in dirty list */
static uint8_t  dirty_list[POOL], n_dirty;
static uint8_t  live_now[POOL], n_live_now;  /* slots with live pixels this frame */
static uint8_t  live_mark[POOL];
static uint8_t  stamp;
static uint8_t  draw_fixed;
static uint8_t  commit_i;

/* VRAM writes are queued and copied during vblank (fast, no waiting for the LCD).
 * Anything that doesn't fit in one vblank is copied in the next one. */
#define QMAX 300u
typedef struct { uint8_t *a; uint8_t v; } qent;
qent q[QMAX];
qent *qp = q;                                          /* next free entry (used by assembly) */
qent *qh = q;   /* (not static: used by assembly) */                                   /* next entry to copy */

/* queue a VRAM write; if the queue is full, write now (slow but safe) */
#define QW(addr, val) do { if (qp != q + QMAX) { qp->a = (addr); qp->v = (val); qp++; } else set_vram_byte((addr), (val)); } while (0)

/* Call right after wait_vbl_done(): copies queued writes while the LCD is in vblank
 * (lines 144-152), about 20 cycles per write; the rest waits for the next vblank. */
void vram_flush(void) __naked
{
    __asm
    ld a, (_qh)
    ld l, a
    ld a, (_qh+1)
    ld h, a
    ld a, (_qp)
    ld c, a
    ld a, (_qp+1)
    ld b, a                     ; BC = end of queue
1$:
    ld a, l
    cp a, c
    jr nz, 2$
    ld a, h
    cp a, b
    jr z, 4$                    ; all copied
2$:
    ldh a, (#0x44)              ; LY
    cp a, #144
    jr c, 3$
    cp a, #153
    jr nc, 3$
    ld a, (hl+)
    ld e, a
    ld a, (hl+)
    ld d, a
    ld a, (hl+)
    ld (de), a
    jr 1$
3$:                             ; vblank over: keep the rest for next frame
    ld a, l
    ld (_qh), a
    ld a, h
    ld (_qh+1), a
    ret
4$:
    ld hl, #_q
    ld a, l
    ld (_qh), a
    ld (_qp), a
    ld a, h
    ld (_qh+1), a
    ld (_qp+1), a
    ret
    __endasm;
}

uint16_t vram_pending(void) { return (uint16_t)(qp - qh); }

void vram_put(uint8_t *a, uint8_t v) { QW(a, v); }

uint8_t hose_ly_end = 128;   /* scanline by which hose work must stop this call */

static const uint16_t ROW_OFF[CELLS_H] = {
    0, 20, 40, 60, 80, 100, 120, 140, 160, 180, 200, 220, 240, 260, 280, 300, 320 };
static uint8_t cur_cx, cur_cy;                         /* cell being looked up (for its map address) */
static const uint8_t BIT[8] = { 0x80, 0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x01 };

void pool_init(void)
{
    uint8_t i;
    memset(cell_slot, NONE, sizeof cell_slot);
    {
        uint8_t *v = (uint8_t *)0x8801u;
        for (i = 0; i < POOL; i++, v += 16) { free_s[i] = (uint8_t)(POOL - 1u - i); slot_lay[i] = lay3[i]; slot_vram[i] = v; }
    }
    n_free = POOL;
    n_active = 0;
    qp = qh = q;
    n_dirty = 0;
    commit_i = 0;
    n_live_now = 0;
    memset(dirty, 0, POOL);
    memset(live_mark, 0, POOL);
    stamp = 1;
}

static void mark_dirty(uint8_t s)
{
    if (dirty[s] != stamp) { dirty[s] = stamp; dirty_list[n_dirty++] = s; }
}

/* Layer bytes for cell c (allocating a pool tile), or 0 if the cell can't hold hose. */
/* Give cell c (at cur_cx, cur_cy) a free pool tile; NONE if the pool is full. */
static uint8_t slot_alloc(uint16_t c)
{
    uint8_t s, *p, *z;
    if (!n_free) return NONE;                          /* pool full: skip */
    s = free_s[--n_free];
    cell_slot[c] = s;
    slot_cell[s] = c;
    slot_map[s] = (uint8_t *)(0x9800u + ((uint16_t)cur_cy << 5) + cur_cx);
    slot_new[s] = 1;
    live_mark[s] = 0;
    p = slot_lay[s];
    for (z = p + 24; z != p; ) *--z = 0;
    active_pos[s] = n_active;
    active[n_active++] = s;
    return s;
}

static uint8_t *cell_layer(uint16_t c)
{
    uint8_t s, *p;
    if (base_tile[c] > B_GROUND + 4u) return 0;        /* rock or pump cell */
    s = cell_slot[c];
    if (s == NONE && (s = slot_alloc(c)) == NONE) return 0;
    if (dirty[s] != stamp) { dirty[s] = stamp; dirty_list[n_dirty++] = s; }
    p = slot_lay[s];
    if (draw_fixed) return p;
    if (live_mark[s] != stamp) { live_mark[s] = stamp; live_now[n_live_now++] = s; }
    return p + 1;
}

static void clear_layer(uint8_t *p)
{
    p[0] = 0; p[3] = 0; p[6] = 0; p[9] = 0; p[12] = 0; p[15] = 0; p[18] = 0; p[21] = 0;
}

/* Clear the live layer of every tile in live_now and put it in the dirty list (assembly;
 * the tiles are distinct and the stamp is fresh, so each one is added exactly once). */
static void clear_live(void) __naked
{
    __asm
    ld a, (_n_live_now)
    or a, a
    ret z
    ld b, a
    ld hl, #_live_now
1$:
    ld a, (hl+)
    push hl
    push bc
    ld c, a
    ld e, a
    ld d, #0
    ld hl, #_dirty
    add hl, de
    ld a, (_stamp)
    ld (hl), a
    ld hl, #_n_dirty
    ld a, (hl)
    inc (hl)
    ld e, a
    ld hl, #_dirty_list
    add hl, de
    ld (hl), c
    ld l, c
    ld h, #0
    add hl, hl
    ld de, #_slot_lay
    add hl, de
    ld a, (hl+)
    ld h, (hl)
    ld l, a
    inc hl                      ; live layer, row 0
    ld de, #3
    xor a, a
    ld (hl), a
    add hl, de
    ld (hl), a
    add hl, de
    ld (hl), a
    add hl, de
    ld (hl), a
    add hl, de
    ld (hl), a
    add hl, de
    ld (hl), a
    add hl, de
    ld (hl), a
    add hl, de
    ld (hl), a
    pop bc
    pop hl
    dec b
    jr nz, 1$
    ret
    __endasm;
}

/* Rough cost of hose_begin() in scanlines, so the caller can wait for a frame with room. */
uint8_t hose_begin_cost(uint8_t rebuild_fixed)
{
    return (uint8_t)((n_live_now >> 1) + (rebuild_fixed ? n_active : 0u) + 4u);
}

/* Start a frame: clear last frame's live pixels (and the fixed layer when rebuilding). */
void hose_begin(uint8_t rebuild_fixed)
{
    uint8_t i, s;
    stamp++;
    if (!stamp) { stamp = 1; memset(dirty, 0, POOL); memset(live_mark, 0, POOL); }
    n_dirty = 0;
    clear_live();                                      /* last frame's live tiles */
    n_live_now = 0;
    if (rebuild_fixed)
        for (i = 0; i < n_active; i++) {
            s = active[i];
            clear_layer(slot_lay[s]);
            mark_dirty(s);
        }
}

/* ------------------------------------------------------------------
 * Line drawing. C sets up each line and handles moving into a new 8x8
 * cell (ln_cell); the per-pixel loops are hand-written SM83 assembly
 * (about 20 CPU cycles per pixel instead of about 100 for compiled C).
 *
 * Lines are always drawn left-to-right (x-major) or top-to-bottom
 * (y-major); only the minor direction varies, giving four small loops.
 * The minor axis advances with an 8-bit fraction: frac += fstep, carry
 * = one step. State shared with the assembly lives in these globals:
 * ------------------------------------------------------------------ */
uint8_t ln_n;          /* pixels left to draw */
uint8_t ln_mask;       /* bit of the current pixel within its tile row */
uint8_t ln_frac;       /* minor-axis fraction */
uint8_t ln_fstep;      /* minor-axis step per pixel (0..255) */
uint8_t ln_xc;         /* current cell column */
uint8_t ln_y;          /* current pixel row */
uint8_t *ln_row;       /* layer byte of the current pixel's tile row (dummy if none) */
static uint8_t ln_dummy[24];  /* stands in for cells that can't hold hose (rows step within it) */

/* Enter the cell (ln_xc, ln_y >> 3): find or allocate its tile, point ln_row at the row. */
uint16_t cl_c;
uint8_t cl_s;
static void cl_alloc(void) { cl_s = slot_alloc(cl_c); }

/* Same as cell_layer() + row pointer, in assembly: this runs every time a line enters a cell. */
void ln_cell(void) __naked
{
    __asm
    ld a, (_ln_xc)
    cp a, #20
    jp nc, 90$
    ld (_cur_cx), a
    ld c, a
    ld a, (_ln_y)
    cp a, #136
    jp nc, 90$
    rrca
    rrca
    rrca
    and a, #0x1F
    ld (_cur_cy), a
    add a, a
    ld l, a
    ld h, #0
    ld de, #_ROW_OFF
    add hl, de
    ld a, (hl+)
    ld h, (hl)
    add a, c
    ld l, a
    jr nc, 1$
    inc h
1$:                             ; HL = cell index
    ld b, h
    ld c, l
    ld de, #_base_tile
    add hl, de
    ld a, (hl)
    cp a, #5
    jp nc, 90$                  ; rock or pump
    ld h, b
    ld l, c
    ld de, #_cell_slot
    add hl, de
    ld a, (hl)
    cp a, #0xFF
    jr nz, 2$
    ld a, c                     ; no tile yet: allocate (C)
    ld (_cl_c), a
    ld a, b
    ld (_cl_c+1), a
    call _cl_alloc
    ld a, (_cl_s)
    cp a, #0xFF
    jp z, 90$
2$:                             ; A = slot
    ld c, a
    ld e, a
    ld d, #0
    ld hl, #_dirty
    add hl, de
    ld a, (_stamp)
    cp a, (hl)
    jr z, 3$
    ld (hl), a
    ld hl, #_n_dirty
    ld a, (hl)
    inc (hl)
    ld e, a
    ld hl, #_dirty_list
    add hl, de
    ld (hl), c
3$:
    ld l, c
    ld h, #0
    add hl, hl
    ld de, #_slot_lay
    add hl, de
    ld a, (hl+)
    ld h, (hl)
    ld l, a                     ; HL = layers of the slot
    ld a, (_draw_fixed)
    or a, a
    jr nz, 5$
    inc hl                      ; live layer
    push hl
    ld e, c
    ld d, #0
    ld hl, #_live_mark
    add hl, de
    ld a, (_stamp)
    cp a, (hl)
    jr z, 4$
    ld (hl), a
    ld hl, #_n_live_now
    ld a, (hl)
    inc (hl)
    ld e, a
    ld hl, #_live_now
    add hl, de
    ld (hl), c
4$:
    pop hl
5$:
    ld a, (_ln_y)               ; + row * 3
    and a, #7
    ld e, a
    add a, a
    add a, e
    ld e, a
    ld d, #0
    add hl, de
    ld a, l
    ld (_ln_row), a
    ld a, h
    ld (_ln_row+1), a
    ret
90$:                            ; outside / rock / pool full: dummy row
    ld hl, #_ln_dummy
    jr 5$
    __endasm;
}

/* x-major, x increasing, y increasing */
static void ln_xmaj_down(void) __naked
{
    __asm
    call 20$
    ret
20$:
    ld a, (_ln_n)
    ld b, a
    ld a, (_ln_mask)
    ld c, a
    ld a, (_ln_frac)
    ld d, a
    ld a, (_ln_y)
    ld e, a
    ld a, (_ln_row)
    ld l, a
    ld a, (_ln_row+1)
    ld h, a
1$:
    ld a, (hl)
    or a, c
    ld (hl), a
    dec b
    ret z
    ld a, (_ln_fstep)
    add a, d
    ld d, a
    jr nc, 3$
    inc e
    inc hl
    inc hl
    inc hl
    ld a, e
    and a, #7
    jr z, 5$
3$:
    srl c
    jr nz, 1$
    ld c, #0x80
    ld a, (_ln_xc)
    inc a
    ld (_ln_xc), a
    jr 6$
5$:
    srl c
    jr nz, 6$
    ld c, #0x80
    ld a, (_ln_xc)
    inc a
    ld (_ln_xc), a
6$:
    ld a, b
    ld (_ln_n), a
    ld a, c
    ld (_ln_mask), a
    ld a, d
    ld (_ln_frac), a
    ld a, e
    ld (_ln_y), a
    call _ln_cell
    jr 20$
    __endasm;
}

/* x-major, x increasing, y decreasing */
static void ln_xmaj_up(void) __naked
{
    __asm
    call 20$
    ret
20$:
    ld a, (_ln_n)
    ld b, a
    ld a, (_ln_mask)
    ld c, a
    ld a, (_ln_frac)
    ld d, a
    ld a, (_ln_y)
    ld e, a
    ld a, (_ln_row)
    ld l, a
    ld a, (_ln_row+1)
    ld h, a
1$:
    ld a, (hl)
    or a, c
    ld (hl), a
    dec b
    ret z
    ld a, (_ln_fstep)
    add a, d
    ld d, a
    jr nc, 3$
    dec e
    dec hl
    dec hl
    dec hl
    ld a, e
    and a, #7
    cp a, #7
    jr z, 5$
3$:
    srl c
    jr nz, 1$
    ld c, #0x80
    ld a, (_ln_xc)
    inc a
    ld (_ln_xc), a
    jr 6$
5$:
    srl c
    jr nz, 6$
    ld c, #0x80
    ld a, (_ln_xc)
    inc a
    ld (_ln_xc), a
6$:
    ld a, b
    ld (_ln_n), a
    ld a, c
    ld (_ln_mask), a
    ld a, d
    ld (_ln_frac), a
    ld a, e
    ld (_ln_y), a
    call _ln_cell
    jr 20$
    __endasm;
}

/* y-major, y increasing, x increasing */
static void ln_ymaj_right(void) __naked
{
    __asm
    call 20$
    ret
20$:
    ld a, (_ln_n)
    ld b, a
    ld a, (_ln_mask)
    ld c, a
    ld a, (_ln_frac)
    ld d, a
    ld a, (_ln_y)
    ld e, a
    ld a, (_ln_row)
    ld l, a
    ld a, (_ln_row+1)
    ld h, a
1$:
    ld a, (hl)
    or a, c
    ld (hl), a
    dec b
    ret z
    inc e
    inc hl
    inc hl
    inc hl
    ld a, e
    and a, #7
    jr z, 5$
    ld a, (_ln_fstep)
    add a, d
    ld d, a
    jr nc, 1$
    srl c
    jr nz, 1$
    ld c, #0x80
    ld a, (_ln_xc)
    inc a
    ld (_ln_xc), a
    jr 6$
5$:
    ld a, (_ln_fstep)
    add a, d
    ld d, a
    jr nc, 6$
    srl c
    jr nz, 6$
    ld c, #0x80
    ld a, (_ln_xc)
    inc a
    ld (_ln_xc), a
6$:
    ld a, b
    ld (_ln_n), a
    ld a, c
    ld (_ln_mask), a
    ld a, d
    ld (_ln_frac), a
    ld a, e
    ld (_ln_y), a
    call _ln_cell
    jr 20$
    __endasm;
}

/* y-major, y increasing, x decreasing */
static void ln_ymaj_left(void) __naked
{
    __asm
    call 20$
    ret
20$:
    ld a, (_ln_n)
    ld b, a
    ld a, (_ln_mask)
    ld c, a
    ld a, (_ln_frac)
    ld d, a
    ld a, (_ln_y)
    ld e, a
    ld a, (_ln_row)
    ld l, a
    ld a, (_ln_row+1)
    ld h, a
1$:
    ld a, (hl)
    or a, c
    ld (hl), a
    dec b
    ret z
    inc e
    inc hl
    inc hl
    inc hl
    ld a, e
    and a, #7
    jr z, 5$
    ld a, (_ln_fstep)
    add a, d
    ld d, a
    jr nc, 1$
    sla c
    jr nz, 1$
    ld c, #0x01
    ld a, (_ln_xc)
    dec a
    ld (_ln_xc), a
    jr 6$
5$:
    ld a, (_ln_fstep)
    add a, d
    ld d, a
    jr nc, 6$
    sla c
    jr nz, 6$
    ld c, #0x01
    ld a, (_ln_xc)
    dec a
    ld (_ln_xc), a
6$:
    ld a, b
    ld (_ln_n), a
    ld a, c
    ld (_ln_mask), a
    ld a, d
    ld (_ln_frac), a
    ld a, e
    ld (_ln_y), a
    call _ln_cell
    jr 20$
    __endasm;
}

/* num * 256 / den for num <= den <= 255 (restoring division, 8 steps), capped at 255 */
static uint8_t frac_div(uint8_t num, uint8_t den)
{
    uint8_t q = 0, i, r = num, top;
    if (num >= den) return 255u;
    for (i = 0; i < 8u; i++) {
        top = r & 0x80u;                               /* r < den <= 255, so 2r fits in 9 bits */
        r <<= 1; q <<= 1;
        if (top || r >= den) { r -= den; q |= 1u; }
    }
    return q;
}

void hose_line(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint8_t fixed)
{
    int16_t t, adx, ady;

    draw_fixed = fixed;
    adx = x1 - x0; if (adx < 0) adx = -adx;
    ady = y1 - y0; if (ady < 0) ady = -ady;
    if (adx >= ady) {
        if (x1 < x0) { t = x0; x0 = x1; x1 = t; t = y0; y0 = y1; y1 = t; }   /* draw left to right */
        if (adx > 255) return;                         /* never happens on screen */
        ln_fstep = adx ? frac_div((uint8_t)ady, (uint8_t)adx) : 0;
        ln_n = (uint8_t)(adx + 1);
    } else {
        if (y1 < y0) { t = x0; x0 = x1; x1 = t; t = y0; y0 = y1; y1 = t; }   /* draw top to bottom */
        if (ady > 255) return;
        ln_fstep = frac_div((uint8_t)adx, (uint8_t)ady);
        ln_n = (uint8_t)(ady + 1);
    }
    ln_frac = 0x80;
    ln_y = (uint8_t)y0;
    ln_xc = (uint8_t)x0 >> 3;
    ln_mask = BIT[(uint8_t)x0 & 7u];
    ln_cell();
    if (adx >= ady) { if (y1 >= y0) ln_xmaj_down(); else ln_xmaj_up(); }
    else            { if (x1 >= x0) ln_ymaj_right(); else ln_ymaj_left(); }
}

/* A hole in the hose: a 3x3 knot on the live layer. */
void hose_blob(int16_t ix, int16_t iy)
{
    uint8_t x, y, *lay;
    draw_fixed = 0;
    for (y = (uint8_t)(iy - 1); y != (uint8_t)(iy + 2); y++)
        for (x = (uint8_t)(ix - 1); x != (uint8_t)(ix + 2); x++) {
            if (x >= SCR_W || y >= PLAY_H) continue;
            cur_cx = x >> 3; cur_cy = y >> 3;
            lay = cell_layer(ROW_OFF[cur_cy] + cur_cx);
            if (lay) lay[(y & 7u) * 3u] |= BIT[x & 7u];
        }
}

/* Compare one tile's 8 rows (fixed | live) with what VRAM shows and queue the changed
 * high-plane bytes. Assembly; arguments and result in globals. */
uint8_t *cr_p;          /* lay3 row 0 */
uint8_t *cr_v;          /* VRAM address of row 0's high-plane byte */
uint8_t cr_any, cr_cnt;

static void commit_rows(void) __naked
{
    __asm
    ld a, (_cr_p)
    ld l, a
    ld a, (_cr_p+1)
    ld h, a
    ld a, (_cr_v)
    ld e, a
    ld a, (_cr_v+1)
    ld d, a
    ld c, #0
    ld a, #8
    ld (_cr_cnt), a
1$:
    ld a, (hl+)
    ld b, a
    ld a, (hl+)
    or a, b
    ld b, a
    or a, c
    ld c, a
    ld a, (hl)
    cp a, b
    jr z, 2$
    ld a, b
    ld (hl), a
    push hl
    ld a, (_qp)
    ld l, a
    ld a, (_qp+1)
    ld h, a
    ld (hl), e
    inc hl
    ld (hl), d
    inc hl
    ld (hl), b
    inc hl
    ld a, l
    ld (_qp), a
    ld a, h
    ld (_qp+1), a
    pop hl
2$:
    inc hl
    inc de
    inc de
    ld a, (_cr_cnt)
    dec a
    ld (_cr_cnt), a
    jr nz, 1$
    ld a, c
    ld (_cr_any), a
    ret
    __endasm;
}

/* First upload of a new tile: queue all 16 bytes (seabed low plane + hose high plane)
 * and remember what was written. cr_b points at the seabed tile's data. */
const uint8_t *cr_b;

static void commit_new(void) __naked
{
    __asm
    ld a, (_cr_v)
    ld e, a
    ld a, (_cr_v+1)
    ld d, a
    dec de                      ; DE = low-plane byte of row 0
    ld c, #0
    ld a, #8
    ld (_cr_cnt), a
1$:
    ld a, (_cr_p)               ; HL = lay3 row
    ld l, a
    ld a, (_cr_p+1)
    ld h, a
    ld a, (hl+)
    ld b, a
    ld a, (hl+)
    or a, b
    ld (hl+), a                 ; shown = fixed | live
    ld b, a
    or a, c
    ld c, a
    ld a, l
    ld (_cr_p), a
    ld a, h
    ld (_cr_p+1), a
    ld a, (_qp)                 ; HL = queue
    ld l, a
    ld a, (_qp+1)
    ld h, a
    ld (hl), e                  ; (low-plane addr, seabed byte)
    inc hl
    ld (hl), d
    inc hl
    push hl
    ld a, (_cr_b)
    ld l, a
    ld a, (_cr_b+1)
    ld h, a
    ld a, (hl+)
    inc hl
    push af
    ld a, l
    ld (_cr_b), a
    ld a, h
    ld (_cr_b+1), a
    pop af
    pop hl
    ld (hl+), a
    inc de                      ; (high-plane addr, hose byte)
    ld (hl), e
    inc hl
    ld (hl), d
    inc hl
    ld (hl), b
    inc hl
    inc de
    ld a, l
    ld (_qp), a
    ld a, h
    ld (_qp+1), a
    ld a, (_cr_cnt)
    dec a
    ld (_cr_cnt), a
    jr nz, 1$
    ld a, c
    ld (_cr_any), a
    ret
    __endasm;
}

/* Push changed tiles to VRAM (via the vblank queue); free tiles that no longer hold hose.
 * Works until the LCD reaches scanline 120 (or the queue is nearly full), then stops;
 * returns 1 when everything is done, 0 if it should be called again next frame. */
static const uint8_t *const GROUND_TILE[5] = {
    &bg_tiles[0], &bg_tiles[16], &bg_tiles[32], &bg_tiles[48], &bg_tiles[64] };

uint8_t hose_commit(void)
{
    uint8_t i, s, last, ly;
    uint16_t c;
    uint8_t *v, *p;
    const uint8_t *b;

    const uint8_t *dl = &dirty_list[commit_i];
    for (i = commit_i; i < n_dirty; i++) {
        ly = LY_REG;
        if ((ly < 144u && ly + 16u >= hose_ly_end) || qp > q + (QMAX - 20u)) { commit_i = i; return 0; }   /* out of time */
        s = *dl++;
        c = slot_cell[s];
        if (cell_slot[c] != s) continue;               /* already freed */
        p = slot_lay[s];
        v = slot_vram[s];
        if (slot_new[s]) {
            b = GROUND_TILE[base_tile[c]];
            cr_p = p; cr_v = v; cr_b = b;
            commit_new();
            if (cr_any) {
                QW(slot_map[s], (uint8_t)(B_POOL + s));
                slot_new[s] = 0;
                continue;
            }
            qp -= 16;                                  /* nothing to show after all: drop the writes */
        } else {
            cr_p = p; cr_v = v;
            commit_rows();
            if (cr_any) continue;
            QW(slot_map[s], base_tile[c]);             /* back to plain seabed */
        }
        cell_slot[c] = NONE;
        slot_new[s] = 0;
        free_s[n_free++] = s;
        last = active[--n_active];                     /* O(1) removal from the active list */
        active[active_pos[s]] = last;
        active_pos[last] = active_pos[s];
    }
    n_dirty = 0;
    commit_i = 0;
    return 1;
}
