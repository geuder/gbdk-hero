/*
 * cone.c - NPC view cones drawn into the background.
 *
 * Cells a cone touches get their own tile from a pool (tile indices 128..191 at 0x8800):
 * a copy of the cell's normal tile with the cone's pixels added in colour 1 (light grey).
 * Only colour-0 pixels are lit, so floor details and walls stay as they are.
 *
 * A cone is a precomputed list of pixel spans, one per row (tools/make_gfx.py). Walls block
 * the view per cell: a cell is lit only if no wall cell lies between it and the eye's cell.
 * Each cone keeps a 7 x 7 cache around its eye cell: 0 = not looked at yet, 0xFF = hidden,
 * otherwise pool slot + 1. The same cache answers "does the NPC see the player?", so what
 * the player sees on screen is exactly what the NPC sees.
 *
 * Cones are drawn again only when an NPC's eye position or facing changed; then all cones
 * are rebuilt and only the tile bytes that changed are queued for VRAM.
 */
#include <gb/gb.h>
#include <stdint.h>
#include <string.h>
#include "game.h"
#include "gfx.h"

#define POOL 64u
#define NONE 0xFFu
#define HIDDEN 0xFFu

uint8_t         cell_slot[PCELLS];      /* cell -> pool slot   (globals here: used by assembly) */
static uint16_t slot_cell[POOL];
static uint8_t *slot_map[POOL];         /* BG map address of the slot's cell */
const uint8_t  *slot_b[POOL];           /* the cell's normal tile data */
uint8_t         lay[POOL * 16u];        /* per slot and tile row: cone pixels, then what VRAM shows */
static uint8_t  slot_new[POOL];
uint8_t         used[POOL];             /* == stamp: touched by this rebuild */
static uint8_t  active[POOL], n_active;
static uint8_t  free_s[POOL], n_free;
uint8_t         stamp;

static uint8_t  vc[NCONES][49];         /* per cone, around the eye cell: 0 = not checked, 1 = visible, 2 = hidden */
static uint16_t vc_eye[NCONES];         /* eye cell the cache is for */
static uint8_t  c_ex[NCONES], c_ey[NCONES], c_k[NCONES];   /* what each cone was drawn with */
static uint8_t  c_on[NCONES];
uint8_t         commit_dirty;           /* (global: used by assembly) */
static uint8_t  phase;

/* state for the row loop (globals: used by the assembly) */
uint8_t cr_ex32;                        /* eye x + 32 (spans are clamped in +32 space) */
uint8_t cr_ecx, cr_cy, cr_x0, cr_x1, cr_rows;
uint8_t *cr_vrow;                       /* cache row for cr_cy */
uint16_t cr_rowp;                       /* padded index of cell (0, cr_cy) */
const uint8_t *cr_p;
uint8_t cr_ecy;                         /* (globals: used by assembly) */
uint16_t cr_ec;                         /* eye cell, padded index */
static uint8_t *cr_vc;

const uint8_t LMASK[8] = { 0xFF, 0x7F, 0x3F, 0x1F, 0x0F, 0x07, 0x03, 0x01 };   /* x&7 .. 7 */
const uint8_t RMASK[8] = { 0x80, 0xC0, 0xE0, 0xF0, 0xF8, 0xFC, 0xFE, 0xFF };   /* 0 .. x&7 */

void cone_reset(void)
{
    uint8_t i;
    memset(cell_slot, NONE, sizeof cell_slot);
    memset(used, 0, sizeof used);
    for (i = 0; i < POOL; i++) free_s[i] = (uint8_t)(POOL - 1u - i);
    n_free = POOL;
    n_active = 0;
    stamp = 1;
    memset(c_on, 0, sizeof c_on);
    memset(c_k, 0xFF, sizeof c_k);
    memset(vc_eye, 0xFF, sizeof vc_eye);
    phase = 0;
    commit_dirty = 0;
}

/* Called from the row loop the first time a cell is looked at (per eye cell): is cell
 * (A, cr_cy) visible from the eye's cell? 1 = visible, 2 = hidden (a wall, or a wall between). */
uint8_t cell_vis(uint8_t cx) __naked
{
    (void)cx;
    __asm
    ld b, a                     ; B = cell x
    ld a, (_cr_rowp)
    add a, b
    ld l, a
    ld a, (_cr_rowp+1)
    adc a, #0
    ld h, a
    ld de, #_solid
    add hl, de
    ld a, (hl)
    or a, a
    jr nz, 9$                   ; the cell itself is a wall
    ld a, (_cr_ecy)             ; table index (cy - ecy + 3) * 7 + (cx - ecx + 3)
    ld c, a
    ld a, (_cr_cy)
    sub a, c
    add a, #3
    ld c, a
    add a, a
    add a, a
    add a, a
    sub a, c
    ld c, a
    ld a, (_cr_ecx)
    cpl
    inc a
    add a, b
    add a, #3
    add a, c
    ld e, a
    ld d, #0
    ld hl, #_los_off
    add hl, de
    ld e, (hl)
    ld hl, #_los_data
    add hl, de
    ld a, (hl+)
    or a, a
    jr z, 8$                    ; nothing in between
    ld b, a
1$:
    ld a, (hl+)
    push hl
    ld e, a                     ; DE = signed offset from the eye cell
    rlca
    sbc a, a
    ld d, a
    ld a, (_cr_ec)
    add a, e
    ld l, a
    ld a, (_cr_ec+1)
    adc a, d
    ld h, a
    ld de, #_solid
    add hl, de
    ld a, (hl)
    pop hl
    or a, a
    jr nz, 9$
    dec b
    jr nz, 1$
8$:
    ld a, #1
    ret
9$:
    ld a, #2
    ret
    __endasm;
}

/* Called from the row loop for a lit cell without a pool tile: take one (NONE if the pool is full). */
uint8_t cell_alloc(uint8_t cx)
{
    uint16_t c = cr_rowp + cx;
    uint8_t s;
    if (!n_free) return NONE;
    s = free_s[--n_free];
    cell_slot[c] = s;
    slot_cell[s] = c;
    slot_map[s] = (uint8_t *)(0x9800u + ((uint16_t)cr_cy << 5) + cx);
    slot_new[s] = 1;
    slot_b[s] = &bg_tiles[(uint16_t)base_tile[c] << 4];
    used[s] = (uint8_t)(stamp - 1u);                   /* (the row loop clears it on first use) */
    active[n_active++] = s;
    return s;
}

/* Rows of one cone: for each row, clamp the span to the screen and OR its mask into every
 * lit cell it crosses. Per cell row ("band") each cell is looked up once (cr_bandp). */
uint8_t cr_c0, cr_lm, cr_rm, cr_r2;     /* first cell, left / right masks, row * 2 */
uint8_t *cr_bandp[7];                   /* per window column: 0 = not looked up, 1 = unlit, else layer */

static void cone_rows_asm(void) __naked
{
    __asm
    call 50$                    ; empty band cache
    ld a, (_cr_p)
    ld l, a
    ld a, (_cr_p+1)
    ld h, a
10$:                            ; ---- row
    ld a, (_cr_ex32)            ; x0 = clamp(ex + rel, 0, 159), done in +32 space
    add a, (hl)
    inc hl
    cp a, #32
    jr nc, 11$
    ld a, #32
11$:
    cp a, #192
    jr c, 12$
    ld a, #191
12$:
    sub a, #32
    ld (_cr_x0), a
    ld a, (_cr_ex32)
    add a, (hl)
    inc hl
    cp a, #32
    jr nc, 13$
    ld a, #32
13$:
    cp a, #192
    jr c, 14$
    ld a, #191
14$:
    sub a, #32
    ld (_cr_x1), a
    push hl
    ld hl, #_cr_x0
    cp a, (hl)
    jp c, 40$                   ; x1 < x0: span is off screen
    ld e, a                     ; right mask = RMASK[x1 & 7]
    and a, #7
    add a, #<(_RMASK)
    ld l, a
    ld a, #>(_RMASK)
    adc a, #0
    ld h, a
    ld a, (hl)
    ld (_cr_rm), a
    ld a, e
    srl a
    srl a
    srl a
    ld c, a                     ; C = last cell column
    ld a, (_cr_x0)              ; left mask = LMASK[x0 & 7]
    ld e, a
    and a, #7
    add a, #<(_LMASK)
    ld l, a
    ld a, #>(_LMASK)
    adc a, #0
    ld h, a
    ld a, (hl)
    ld (_cr_lm), a
    ld a, e
    srl a
    srl a
    srl a
    ld b, a                     ; B = first cell column
    ld (_cr_c0), a
20$:                            ; ---- cell B of this row
    ld a, (_cr_ecx)
    cpl
    inc a
    add a, b
    add a, #3                   ; window column 0..6
    cp a, #7
    jr nc, 30$
    ld e, a
    add a, a
    add a, #<(_cr_bandp)
    ld l, a
    ld a, #>(_cr_bandp)
    adc a, #0
    ld h, a                     ; HL = &cr_bandp[col]
    ld a, (hl+)
    ld d, (hl)
    or a, d
    jr nz, 21$
    push hl                     ; first time in this band: look the cell up
    push bc
    call 60$                    ; B = cell x, E = window column -> DE = layer or 1
    pop bc
    pop hl
    ld a, d
    ld (hl-), a
    ld (hl), e
    jr 22$
21$:
    dec hl
    ld e, (hl)                  ; DE = layer or 1
22$:
    ld a, d
    or a, a
    jr z, 30$                   ; unlit
    ld a, (_cr_r2)              ; HL = layer + row * 2
    add a, e
    ld l, a
    ld a, d
    adc a, #0
    ld h, a
    ld d, #0xFF                 ; D = mask
    ld a, (_cr_c0)
    cp a, b
    jr nz, 23$
    ld a, (_cr_lm)
    ld d, a
23$:
    ld a, c
    cp a, b
    jr nz, 25$
    ld a, (_cr_rm)
    and a, d
    ld d, a
25$:
    ld a, (hl)
    or a, d
    ld (hl), a
30$:                            ; next cell
    ld a, b
    inc b
    cp a, c
    jr c, 20$
40$:                            ; ---- next row
    pop hl
    ld a, (_cr_r2)
    add a, #2
    and a, #15
    ld (_cr_r2), a
    jr nz, 41$
    push hl                     ; new band
    call 50$
    pop hl
    ld a, (_cr_cy)
    inc a
    ld (_cr_cy), a
    ld a, (_cr_rowp)
    add a, #22                  ; PW
    ld (_cr_rowp), a
    jr nc, 42$
    ld a, (_cr_rowp+1)
    inc a
    ld (_cr_rowp+1), a
42$:
    ld a, (_cr_vrow)
    add a, #7
    ld (_cr_vrow), a
    jr nc, 41$
    ld a, (_cr_vrow+1)
    inc a
    ld (_cr_vrow+1), a
41$:
    ld a, (_cr_rows)
    dec a
    ld (_cr_rows), a
    jp nz, 10$
    ret

50$:                            ; ---- empty the band cache
    ld hl, #_cr_bandp
    xor a, a
    .rept 14
    ld (hl+), a
    .endm
    ret

60$:                            ; ---- look up cell B (window column E): DE = layer, or 1 if unlit
    ld d, #0
    ld a, (_cr_vrow)
    ld l, a
    ld a, (_cr_vrow+1)
    ld h, a
    add hl, de
    ld a, (hl)
    or a, a
    jr nz, 61$
    push bc                     ; first look from this eye cell
    push hl
    ld a, b
    call _cell_vis
    pop hl
    pop bc
    ld (hl), a
61$:
    dec a
    jr nz, 69$                  ; hidden
    ld a, (_cr_rowp)            ; A = cell_slot[row + B]
    add a, b
    ld l, a
    ld a, (_cr_rowp+1)
    adc a, #0
    ld h, a
    ld de, #_cell_slot
    add hl, de
    ld a, (hl)
    cp a, #0xFF
    jr nz, 62$
    push bc
    ld a, b
    call _cell_alloc
    pop bc
    cp a, #0xFF
    jr z, 69$                   ; pool full
62$:
    ld e, a
    ld d, #0                    ; DE = slot
    ld hl, #_used
    add hl, de
    ld a, (_stamp)
    cp a, (hl)
    push af
    ld (hl), a
    ld h, d                     ; HL = lay + slot * 16
    ld l, e
    add hl, hl
    add hl, hl
    add hl, hl
    add hl, hl
    ld de, #_lay
    add hl, de
    ld d, h
    ld e, l
    pop af
    ret z                       ; already in use this rebuild
    xor a, a                    ; first use: clear the cone rows
    .rept 8
    ld (hl+), a
    inc hl
    .endm
    ret
69$:
    ld de, #1
    ret
    __endasm;
}

/* Draw cone n into the layers; eye (ex, ey), facing k. */
static void draw_one(uint8_t n)
{
    const uint8_t *p = &cone_data[cone_off[c_k[n]]];
    int16_t py = (int16_t)c_ey[n] + (int8_t)p[0];
    uint8_t rows = p[1], ey = c_ey[n];
    int16_t wy;

    p += 2;
    cr_vc = vc[n];
    while (rows && py < 0) { py++; rows--; p += 2; }     /* rows above the screen */
    if ((int16_t)(py + rows) > (int16_t)SCR_H) rows = (uint8_t)(SCR_H - py);
    if (!rows || py >= (int16_t)SCR_H) return;
    cr_p = p;
    cr_rows = rows;
    cr_ex32 = (uint8_t)(c_ex[n] + 32u);
    cr_ecx = c_ex[n] >> 3;
    cr_ecy = ey >> 3;
    cr_ec = ROWP[cr_ecy] + cr_ecx;
    if (vc_eye[n] != cr_ec) { vc_eye[n] = cr_ec; memset(cr_vc, 0, 49); }   /* new eye cell */
    cr_cy = (uint8_t)py >> 3;
    cr_r2 = (uint8_t)(((uint8_t)py & 7u) << 1);
    cr_rowp = ROWP[cr_cy];
    wy = (int16_t)cr_cy - cr_ecy + 3;                   /* cache row of the first row */
    cr_vrow = cr_vc + wy * 7;                          /* rows are within +-20 px: always in the window */
    cone_rows_asm();
}

/* commit helpers (assembly): cm_b = normal tile data, cm_l = slot's layer rows,
 * cm_v = VRAM address of the pool tile's row 0 */
const uint8_t *cm_b;
uint8_t *cm_l, *cm_v;

/* Changed rows only: new low plane = tile low | cone (floor tiles use colours 0 and 1 only,
 * so their high plane is 0); queue it if it differs from what VRAM shows. */
static void commit_rows(void) __naked
{
    __asm
    ld a, (_cm_l)
    ld l, a
    ld a, (_cm_l+1)
    ld h, a
    ld a, (_cm_b)
    ld e, a
    ld a, (_cm_b+1)
    ld d, a
    ld a, (_cm_v)
    ld c, a                     ; C = VRAM address, low byte
    ld b, #8
1$:
    ld a, (de)
    inc de
    inc de
    or a, (hl)
    inc hl
    cp a, (hl)
    jr z, 2$
    ld (hl), a
    push hl
    push de
    ld e, a
    ld a, (_qp)
    ld l, a
    ld a, (_qp+1)
    ld h, a
    ld (hl), c
    inc hl
    ld a, (_cm_v+1)
    ld (hl+), a
    ld (hl), e
    inc hl
    ld a, l
    ld (_qp), a
    ld a, h
    ld (_qp+1), a
    pop de
    pop hl
2$:
    inc hl
    inc c
    inc c
    dec b
    jr nz, 1$
    ret
    __endasm;
}

/* New tile: queue all 16 bytes and remember the low plane. */
static void commit_new(void) __naked
{
    __asm
    ld a, (_qp)
    ld l, a
    ld a, (_qp+1)
    ld h, a                     ; HL = queue
    ld a, (_cm_b)
    ld e, a
    ld a, (_cm_b+1)
    ld d, a                     ; DE = tile data
    ld a, (_cm_v)
    ld c, a                     ; C = VRAM address low byte
    ld b, #8
1$:
    push bc
    ld a, (_cm_l)               ; B = tile low | cone, remembered as shown
    ld c, a
    ld a, (_cm_l+1)
    ld b, a
    ld a, (de)
    inc de
    push hl
    ld h, b
    ld l, c
    or a, (hl)
    inc hl
    ld (hl+), a
    ld b, a
    ld a, l
    ld (_cm_l), a
    ld a, h
    ld (_cm_l+1), a
    pop hl
    ld a, b
    pop bc
    push af
    ld (hl), c                  ; (v, low plane)
    inc hl
    ld a, (_cm_v+1)
    ld (hl+), a
    pop af
    ld (hl+), a
    inc c
    ld (hl), c                  ; (v + 1, high plane)
    inc hl
    ld a, (_cm_v+1)
    ld (hl+), a
    ld a, (de)
    inc de
    ld (hl+), a
    inc c
    dec b
    jr nz, 1$
    ld a, l
    ld (_qp), a
    ld a, h
    ld (_qp+1), a
    ret
    __endasm;
}

/* Slot no cone uses any more (active[i]): give its cell the normal tile back, free it. */
void slot_free(uint8_t i)
{
    uint8_t s = active[i];
    uint16_t c = slot_cell[s];
    if (!slot_new[s]) QW(slot_map[s], base_tile[c]);
    cell_slot[c] = NONE;
    free_s[n_free++] = s;
    active[i] = active[--n_active];
}

/* Slot used for the first time: the whole tile, then point the map at it. */
void slot_first(uint8_t s)
{
    cm_b = slot_b[s];
    cm_v = (uint8_t *)(0x8800u + ((uint16_t)s << 4));
    cm_l = &lay[(uint16_t)s << 4];
    commit_new();
    QW(slot_map[s], (uint8_t)(B_POOL + s));
    slot_new[s] = 0;
}

uint8_t cm_i;

/* Queue the changed tiles; give back tiles no cone touches any more. If the VRAM queue gets
 * too full, stop and finish next frame (comparing with what's shown makes that safe). */
static void commit(void) __naked
{
    __asm
    xor a, a
    ld (_commit_dirty), a
    ld a, (_n_active)
    ld (_cm_i), a
1$:
    ld a, (_cm_i)
    or a, a
    ret z
    dec a
    ld (_cm_i), a
    ld a, (_qp+1)               ; room for a whole tile in the queue?
    cp a, #>(_q + 3 * (400 - 20))
    jr c, 2$
    jp nz, 9$
    ld a, (_qp)
    cp a, #<(_q + 3 * (400 - 20))
    jp nc, 9$
2$:
    ld a, (_cm_i)
    add a, #<(_active)
    ld l, a
    ld a, #>(_active)
    adc a, #0
    ld h, a
    ld a, (hl)                  ; A = slot
    ld e, a
    ld d, #0
    ld hl, #_used
    add hl, de
    ld a, (_stamp)
    cp a, (hl)
    jr z, 3$
    ld a, (_cm_i)               ; unused now
    call _slot_free
    jp 1$
3$:
    ld hl, #_slot_new
    add hl, de
    ld a, (hl)
    or a, a
    jr z, 4$
    ld a, e
    call _slot_first
    jp 1$
4$:                             ; ---- changed rows of slot E
    ld hl, #_slot_b             ; slot_b[s] = tile data
    add hl, de
    add hl, de
    ld a, (hl+)
    ld (_cm_b), a
    ld a, (hl)
    ld (_cm_b+1), a
    ld a, e                     ; VRAM 0x8800 + s * 16
    swap a
    ld c, a
    and a, #0xF0
    ld (_cm_v), a
    ld a, c
    and a, #0x0F
    add a, #0x88
    ld (_cm_v+1), a
    ld h, d                     ; HL = lay + s * 16
    ld l, e
    add hl, hl
    add hl, hl
    add hl, hl
    add hl, hl
    ld de, #_lay
    add hl, de
    ld a, (_cm_b)
    ld e, a
    ld a, (_cm_b+1)
    ld d, a
    ld a, (_cm_v)
    ld c, a
    ld b, #8
5$:
    ld a, (de)
    inc de
    inc de
    or a, (hl)
    inc hl
    cp a, (hl)
    jr z, 6$
    ld (hl), a
    push hl
    push de
    ld e, a
    ld a, (_qp)
    ld l, a
    ld a, (_qp+1)
    ld h, a
    ld (hl), c
    inc hl
    ld a, (_cm_v+1)
    ld (hl+), a
    ld (hl), e
    inc hl
    ld a, l
    ld (_qp), a
    ld a, h
    ld (_qp+1), a
    pop de
    pop hl
6$:
    inc hl
    inc c
    inc c
    dec b
    jr nz, 5$
    jp 1$
9$:
    ld a, #1
    ld (_commit_dirty), a
    ret
    __endasm;
}

/* The cones are redrawn in steps, one per frame, so no frame gets all of the work:
 *   step 0: notice a change, take a snapshot of all cones, draw cone 0
 *   step 1: draw cone 1
 *   step 2: upload the changed tiles
 * The snapshot is also what cone_sees() uses, so detection matches the drawn cones. */

void cones_update(const uint8_t *on, const uint8_t *ex, const uint8_t *ey, const uint8_t *k)
{
    uint8_t n, changed = 0;
    if (phase == 1) { if (c_on[1]) draw_one(1); phase = 2; return; }
    if (phase == 2) { commit(); phase = 0; return; }
    for (n = 0; n < NCONES; n++)
        if (on[n] != c_on[n] || (on[n] && (ex[n] != c_ex[n] || ey[n] != c_ey[n] || k[n] != c_k[n]))) changed = 1;
    if (!changed) {
        if (commit_dirty) commit();                    /* finish uploads that didn't fit */
        return;
    }
    if (!++stamp) { stamp = 1; memset(used, 0, sizeof used); }
    for (n = 0; n < NCONES; n++) { c_on[n] = on[n]; c_ex[n] = ex[n]; c_ey[n] = ey[n]; c_k[n] = k[n]; }
    if (c_on[0]) draw_one(0);
    phase = 1;
}

/* Does cone n light any pixel of the 8 x 13 box at (bx, by)? Uses the drawn cone's spans and
 * visibility cache, so it matches what's on screen. */
uint8_t cone_sees(uint8_t n, uint8_t bx, uint8_t by)
{
    const uint8_t *p;
    uint8_t rows, y, y1, top, lo, hi, x0, x1, ex32, ex, ey, rx, ry, v, *vrow;
    if (!c_on[n]) return 0;
    ex = c_ex[n]; ey = c_ey[n];
    if ((uint8_t)(bx + 8u + CONE_LEN - ex) > (uint8_t)(8u + 2u * CONE_LEN)) return 0;   /* quick reject */
    if ((uint8_t)(by + 13u + CONE_LEN - ey) > (uint8_t)(13u + 2u * CONE_LEN)) return 0;
    p = &cone_data[cone_off[c_k[n]]];
    top = (uint8_t)(ey + (int8_t)p[0]);                /* screen row of the cone's first row */
    rows = p[1];
    p += 2;
    if ((int8_t)p[-2] + (int16_t)ey < 0) return 0;    /* (cone partly above the screen: skip, rare) */
    y = by > top ? by : top;
    y1 = (uint8_t)(top + rows - 1u);
    if (y1 > (uint8_t)(by + 12u)) y1 = (uint8_t)(by + 12u);
    ex32 = (uint8_t)(ex + 32u);
    for (p += (uint8_t)(y - top) << 1; y <= y1; y++, p += 2) {
        x0 = (uint8_t)(ex32 + p[0]); x1 = (uint8_t)(ex32 + p[1]);       /* +32 space */
        lo = (uint8_t)(bx + 32u); hi = (uint8_t)(bx + 39u);
        if (x0 > lo) lo = x0;
        if (x1 < hi) hi = x1;
        if (lo > hi) continue;
        ry = (uint8_t)((y >> 3) - (ey >> 3) + 3u);
        vrow = &vc[n][(uint8_t)(ry * 7u)];
        rx = (uint8_t)(((uint8_t)(lo - 32u) >> 3) - (ex >> 3) + 3u);
        if (rx < 7u) { v = vrow[rx]; if (v == 1u) return 1; }
        rx = (uint8_t)(((uint8_t)(hi - 32u) >> 3) - (ex >> 3) + 3u);
        if (rx < 7u) { v = vrow[rx]; if (v == 1u) return 1; }
    }
    return 0;
}
