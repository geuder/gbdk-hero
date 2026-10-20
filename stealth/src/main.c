/*
 * main.c - stealth game for the Game Boy (DMG), GBDK-2020.
 *
 * A port of the browser prototype (web/template.html): sneak past or take down the guards.
 *
 *   - The player moves in 8 directions with the D-pad; A takes down a guard up close.
 *   - Guards have a dithered view cone (60 px, 30 degrees); walls block it, the ravine doesn't.
 *   - The closer you are when a guard sees you, the sooner it attacks: 2.25 s at the far end
 *     of the cone, down to nothing right in front of it.
 *   - Guard states:
 *       NORMAL      doing their duty (a route script, or a post)
 *       SUSPICIOUS  glimpsed the player (or found a body): search at random nearby
 *       ATTACK      saw the player long enough: go to the last known position, kill on arrival
 *       DOWN        knocked out for 20 s, then back up and suspicious
 *   - Five levels; reach the exit (level 5: take everyone down for now).
 *
 * Credits: hero sprites "8-Directional Game Boy Character Template" by GibbonGL (CC0),
 * https://gibbongl.itch.io/8-directional-gameboy-character-template
 */
#include <gb/gb.h>
#include <stdint.h>
#include <string.h>
#include "game.h"
#include "gfx.h"
#include "world.h"

/* ---- tuning (as in the browser prototype) ----
 * NPCs are updated every other frame (half of them each frame), so their per-update steps and
 * timers below are for 2 frames: the speeds and times in seconds are the prototype's. */
#define PLAYER_SPEED   192u     /* 8.8 fixed point px per frame (0.75) */
#define PLAYER_DIAG    136u     /* x 1/sqrt(2) */
#define ATTACK_SPEED   512u
#define SPOT_FULL      2160u    /* detection meter: 135 frames x 16 at the cone's far end */
#define SPOT_DECAY     32u      /* per update out of sight */
#define SEARCH_FRAMES  240u     /* updates: 8 s of searching, then back to duty */
#define SEARCH_RANGE   3u       /* cells around the spot where the player was glimpsed */
#define KILL_DIST      9u       /* px between feet */
#define TAKEDOWN_DIST  12u
#define TOUCH_X        8u       /* feet this close = bumping into the NPC */
#define TOUCH_Y        6u
#define LOOK_RADIUS    CONE_LEN /* "look again" on arrival */
#define CONE_LY        134u     /* cone work stops this many lines after vblank started (scan line 124) */
#define CONE_LY_WALLS  118u     /* (levels with walls: a line-of-sight check can take a while) */
#define SWAY_STEP      874u     /* look-around sway: one cycle per 150 frames (per update) */

uint8_t solid[PCELLS];
uint8_t base_tile[PCELLS];
const uint16_t ROWP[CELLS_H + 1u] = {
    PIDX(0, 0), PIDX(0, 1), PIDX(0, 2), PIDX(0, 3), PIDX(0, 4), PIDX(0, 5),
    PIDX(0, 6), PIDX(0, 7), PIDX(0, 8), PIDX(0, 9), PIDX(0, 10), PIDX(0, 11),
    PIDX(0, 12), PIDX(0, 13), PIDX(0, 14), PIDX(0, 15), PIDX(0, 16), PIDX(0, 17) };

/* ---- game state (global so the map file / emulator can watch them) ---- */

uint8_t gstate, level_no, sel_level, nnpc;
const Level *lv;
uint16_t frame;
uint8_t keys, prev_keys;
uint16_t pfx, pfy;              /* player feet centre, 8.8 */
uint8_t pdir, pstep, panim;
uint8_t signals[2];             /* guard rotation: bumps sent so far */
uint8_t n_down;                 /* NPCs knocked out */
uint8_t lag_frames;             /* frames the work didn't fit in (for testing) */
#ifdef PROF                     /* make CFLAGS+=-DPROF: time per part of the frame, in DIV ticks (64 cycles) */
uint16_t prof[20];              /* in scan lines (114 cycles); the frame is 154 lines */
uint16_t prof_t;
uint8_t prof_over, prof_wrap, prof_max;
uint8_t prof_log[64], prof_li;
uint16_t prof_now(void) { uint8_t l, t; do { t = (uint8_t)sys_time; l = LY_REG; } while (t != (uint8_t)sys_time); return (uint16_t)t * 154u + (l >= 144u ? l - 144u : l + 10u); }   /* sys_time counts at line 144 */
uint8_t prof_pos[20], prof_worst[20];
void prof_mark(uint8_t i) { uint16_t t_ = prof_now(); { uint8_t l = LY_REG; prof_pos[i] = l >= 144u ? (uint8_t)(l - 144u) : (uint8_t)(l + 10u); } prof[i] += (uint16_t)(t_ >= prof_t ? t_ - prof_t : t_ + 39424u - prof_t); prof_t = t_; }
#define PROF_MARK(i) prof_mark(i)
#else
#define PROF_MARK(i)
#endif

Npc npc[NNPC];

/* ---- small maths ---- */
static uint16_t rng = 0xACE1u;
uint8_t rnd(void) { rng = rng * 25173u + 13849u; return (uint8_t)(rng >> 8); }

static const uint8_t ATAN_T[33] = {     /* atan(i/32) in 1/256 turns */
    0, 1, 3, 4, 5, 6, 8, 9, 10, 11, 12, 13, 15, 16, 17, 18,
    19, 20, 21, 22, 23, 24, 25, 25, 26, 27, 28, 29, 29, 30, 31, 31, 32 };
uint8_t atan2b(int16_t dx, int16_t dy)
{
    uint16_t ax = dx < 0 ? (uint16_t)-dx : (uint16_t)dx, ay = dy < 0 ? (uint16_t)-dy : (uint16_t)dy;
    uint8_t a;
    while (ax > 255u || ay > 255u) { ax >>= 1; ay >>= 1; }
    if (!ax && !ay) return 0;
    if (ax >= ay) a = ATAN_T[(uint8_t)((ay << 5) / ax)];
    else a = (uint8_t)(64u - ATAN_T[(uint8_t)((ax << 5) / ay)]);
    if (dx < 0) a = (uint8_t)(128u - a);
    if (dy < 0) a = (uint8_t)(0u - a);
    return a;
}
static const int8_t SWAY_T[32] = {      /* 28 * sin: +-10 degrees in quarter facing units */
    0, 5, 11, 16, 20, 23, 26, 27, 28, 27, 26, 23, 20, 16, 11, 5,
    0, -5, -11, -16, -20, -23, -26, -27, -28, -27, -26, -23, -20, -16, -11, -5 };
static const uint8_t ROW_FROM_OCT[8] = { 6, 7, 0, 1, 2, 3, 4, 5 };   /* facing octant -> sheet row */
static const uint8_t DIR_FROM[3][3] = { { 3, 4, 5 }, { 2, 0, 6 }, { 1, 0, 7 } };

uint8_t ad(uint8_t a, uint8_t b) { return a > b ? a - b : b - a; }

uint8_t cell_solid(uint8_t x, uint8_t y) { return solid[ROWP[y >> 3] + (x >> 3)]; }

/* Line of sight between two points (walls block it, the ravine doesn't). */
static uint8_t los(uint8_t x0, uint8_t y0, uint8_t x1, uint8_t y1)
{
    int16_t dx = (int16_t)x1 - x0, dy = (int16_t)y1 - y0, sx, sy, x, y;
    uint8_t n = ad(x0, x1) > ad(y0, y1) ? ad(x0, x1) : ad(y0, y1), i;
    if (!n || los_open) return 1;
    sx = (dx << 8) / n; sy = (dy << 8) / n;
    x = (int16_t)x0 << 8; y = (int16_t)y0 << 8;
    for (i = 0; i < n; i++) {
        x += sx; y += sy;
        if ((uint8_t)(y >> 8) >= PLAY_H) return 0;
        if (cell_solid((uint8_t)(x >> 8), (uint8_t)(y >> 8)) & OPAQUE) return 0;
    }
    return 1;
}

/* ---- text ---- */
static const char FONT_CHARS[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!,.-:";
uint8_t rowbuf[20];
void text_row(const char *s)
{
    uint8_t i, n = (uint8_t)strlen(s), x0 = (uint8_t)((20u - n) >> 1);
    const char *f;
    memset(rowbuf, B_FONT, 20);
    for (i = 0; i < n; i++) {
        for (f = FONT_CHARS; *f && *f != s[i]; f++) ;
        rowbuf[x0 + i] = (uint8_t)(B_FONT + (f - FONT_CHARS));
    }
}
void win_row(uint8_t n)                       /* rowbuf -> window row n (queued) */
{
    uint8_t i, *w = (uint8_t *)(0x9C00u + ((uint16_t)n << 5));
    for (i = 0; i < 20u; i++) vram_put(w + i, rowbuf[i]);
}
static char numbuf[24];
static const char *level_label(uint8_t i)            /* "4 GUARD ROTATION" */
{
    numbuf[0] = (char)('1' + i); numbuf[1] = ' ';
    strcpy(numbuf + 2, levels[i].name);
    return numbuf;
}
static void bar_text(void) { text_row(level_label(level_no)); win_row(0); }
static void banner(const char *a, const char *b)    /* 4 rows of window at the bottom */
{
    text_row(""); win_row(0);
    text_row(a); win_row(1);
    text_row(b); win_row(2);
    text_row(""); win_row(3);
    WY_REG = (uint8_t)(SCR_H - 32u);
}

/* ---- path finding: breadth-first distance field from the destination ----
 * There are fewer fields than NPCs: an NPC holds one only while it walks somewhere (it gives it
 * back on arrival). If none is free, it waits a moment. */
#define NFIELD 3u
#define BFS_Q  128u                    /* ring queue of cells (pointers into the field) */
static uint8_t field[NFIELD][PCELLS];
static uint8_t field_owner[NFIELD];    /* NPC, or 0xFF */
uint8_t *bfs_q[BFS_Q];
uint8_t **bfs_head, **bfs_tail;
uint8_t *bfs_goal;                     /* the NPC's cell */
uint8_t bfs_stop;                      /* distance of the NPC's cell, once reached */
uint8_t *bfs_field;
static uint8_t bfs_pending;            /* bit per NPC */

/* ---- map ---- */
static uint8_t map_ch(int8_t x, int8_t y)
{
    if (x < 0 || y < 0 || x >= (int8_t)CELLS_W || y >= (int8_t)CELLS_H) return '#';
    return (uint8_t)lv->map[y][x];
}
static const uint8_t TUFTS[64] = {      /* grass tuft tile per scatter index (0 two tufts, 1 one, 2 none) */
    2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 0, 0, 2, 2, 2, 0, 2, 2, 2, 1, 2, 2, 0, 2, 1, 1, 2, 2, 1, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 1, 0, 0, 2, 2, 0, 2, 1 };

static void build_map(void)
{
    uint8_t x, y, t, m, ry, rx;
    uint16_t c;
    const char *row;
    memset(solid, 3, sizeof solid);
    memset(base_tile, B_WALL, sizeof base_tile);
    los_open = 1;
    for (y = 0, ry = 0; y < CELLS_H; y++, ry += 13u) {
        row = lv->map[y];
        c = ROWP[y];
        for (x = 0, rx = 0; x < CELLS_W; x++, c++, rx += 7u) {
            switch (row[x]) {
                case '#': t = B_WALL; los_open = 0; break;
                case ',': t = (uint8_t)(B_FLOOR + (y & 1u)); solid[c] = 0; break;
                case 'o':                              /* ravine: lit edges where the ground is */
                    m = 0;
                    if (map_ch((int8_t)x, (int8_t)y - 1) != 'o' && y) m |= 1u;
                    if (map_ch((int8_t)x - 1, (int8_t)y) != 'o' && x) m |= 2u;
                    if (map_ch((int8_t)x + 1, (int8_t)y) != 'o' && x + 1u < CELLS_W) m |= 4u;
                    if (map_ch((int8_t)x, (int8_t)y + 1) != 'o' && y + 1u < CELLS_H) m |= 8u;
                    t = (uint8_t)(B_PIT + m); solid[c] = 1; break;
                default:                               /* scatter the grass tufts */
                    t = (uint8_t)(B_GROUND + TUFTS[(uint8_t)(rx + ry) & 63u]);
                    solid[c] = 0; break;
            }
            base_tile[c] = t;
        }
    }
    for (t = 0; t < lv->nbush; t++) {                  /* bushes: tiles per placement offset (x or y +4) */
        const Box *b = &lv->bush[t];
        uint8_t w = (b->x & 7u) ? 4u : 3u, h = (b->y & 7u) ? 4u : 3u, tile = (b->x & 7u) ? B_BUSH_X : (b->y & 7u) ? B_BUSH_Y : B_BUSH;
        for (y = 0; y < h; y++)
            for (x = 0; x < w; x++, tile++)
                if ((b->x >> 3) + x < CELLS_W && (b->y >> 3) + y < CELLS_H) base_tile[PIDX((b->x >> 3) + x, (b->y >> 3) + y)] = tile;
    }
}
#define IS_BUSH(t) ((uint8_t)((t) - B_BUSH) < (uint8_t)(B_ICON - B_BUSH))
uint8_t in_bush(uint8_t x, uint8_t y)
{
    uint8_t k;
    const Box *b = lv->bush;
    if (!IS_BUSH(base_tile[ROWP[y >> 3] + (x >> 3)])) return 0;   /* (quick: not on a bush cell) */
    for (k = lv->nbush; k; k--, b++)
        if ((uint8_t)(x - b->x) < b->w && (uint8_t)(y - b->y) < b->h) return 1;
    return 0;
}
static uint8_t near_bush(uint8_t x, uint8_t y)         /* a 16 x 16 figure with feet at (x, y) overlaps a bush cell */
{
    const uint8_t *r0, *r1;                            /* (a bush is 3 cells wide: checking the corners is enough) */
    uint8_t x0 = x >= 8u ? (uint8_t)((x - 8u) >> 3) : 0u, x1 = (uint8_t)((x + 7u) >> 3);
    if (!lv->nbush) return 0;
    r0 = &base_tile[ROWP[y >= 13u ? (uint8_t)((y - 13u) >> 3) : 0u]];
    r1 = &base_tile[ROWP[(uint8_t)(y + 2u) >> 3]];
    return IS_BUSH(r0[x0]) || IS_BUSH(r0[x1]) || IS_BUSH(r1[x0]) || IS_BUSH(r1[x1]);
}
static void draw_map(void)            /* LCD off */
{
    uint8_t y, row[20], x;
    for (y = 0; y < CELLS_H; y++) {
        for (x = 0; x < CELLS_W; x++) row[x] = base_tile[PIDX(x, y)];
        set_bkg_tiles(0, y, 20, 1, row);
    }
}

static uint8_t bfs_cur = 0xFF;          /* NPC whose field is being built */

/* Field starts as 254 = blocked, 255 = not reached yet (from solid[]). */
static void field_init(void) __naked
{
    __asm
    ld hl, #_solid
    ld a, (_bfs_field)
    ld e, a
    ld a, (_bfs_field+1)
    ld d, a
    ld bc, #((418 + 1) / 2)     ; PCELLS, two at a time
1$:
    ld a, (hl+)
    cp a, #1
    ld a, #255
    adc a, #-1                  ; open (0): carry -> 255, blocked: 254
    ld (de), a
    inc de
    ld a, (hl+)
    cp a, #1
    ld a, #255
    adc a, #-1
    ld (de), a
    inc de
    dec bc
    ld a, b
    or a, c
    jr nz, 1$
    ret
    __endasm;
}

static void bfs_start(uint8_t n)
{
    uint8_t *d = field[npc[n].field], *t;
    bfs_field = d;
    field_init();
    t = &d[ROWP[npc[n].ty >> 3] + (npc[n].tx >> 3)];
    *t = 0;
    bfs_head = bfs_tail = bfs_q;
    *bfs_tail++ = t;
    bfs_goal = &d[ROWP[PX(npc[n].fy) >> 3] + (PX(npc[n].fx) >> 3)];
    bfs_stop = bfs_goal == t ? 0u : 255u;
    bfs_cur = n;
}

/* Breadth-first distances from the destination (assembly). Stops when every cell up to the
 * NPC's own distance is done (enough to walk the shortest way), or when the frame is nearly
 * over (then it continues next frame). Returns 1 when done. */
static uint8_t bfs_run(void) __naked
{
    __asm
1$:
    ld a, (_bfs_head)
    ld l, a
    ld a, (_bfs_head+1)
    ld h, a
    ld a, (_bfs_tail)
    cp a, l
    jr nz, 2$
    ld a, (_bfs_tail+1)
    cp a, h
    jr z, 8$                    ; queue empty: done
2$:
    ldh a, (#0x40)              ; out of time? (only with the LCD on)
    bit 7, a
    jr z, 3$
    ldh a, (#0x44)
    cp a, #112                  ; (leave the rest of the frame to the cones)
    jr c, 3$
    cp a, #144                  ; (in vblank: fine)
    jr nc, 3$
    xor a, a
    ret
3$:
    ld a, (hl+)                 ; DE = next cell
    ld e, a
    ld a, (hl+)
    ld d, a
    ld a, l                     ; (ring: wrap at the end)
    cp a, #<(_bfs_q + 256)
    jr nz, 5$
    ld a, h
    cp a, #>(_bfs_q + 256)
    jr nz, 5$
    ld hl, #_bfs_q
5$:
    ld a, l
    ld (_bfs_head), a
    ld a, h
    ld (_bfs_head+1), a
    ld a, (_bfs_stop)
    ld b, a
    ld a, (de)
    cp a, b
    jr z, 4$
    jr nc, 8$                   ; past the NPC distance: done
4$:
    inc a
    ld c, a                     ; C = distance of the neighbours
    ld h, d                     ; left
    ld l, e
    dec hl
    call 20$
    ld h, d                     ; right
    ld l, e
    inc hl
    call 20$
    ld a, e                     ; up (-22)
    sub a, #22
    ld l, a
    ld a, d
    sbc a, #0
    ld h, a
    call 20$
    ld a, e                     ; down (+22)
    add a, #22
    ld l, a
    ld a, d
    adc a, #0
    ld h, a
    call 20$
    jr 1$
8$:
    ld a, #1
    ret
20$:                            ; neighbour HL: not reached yet (255)? set it, queue it
    ld a, (hl)
    inc a
    ret nz
    ld (hl), c
    push de
    ld d, h
    ld e, l
    ld a, (_bfs_tail)
    ld l, a
    ld a, (_bfs_tail+1)
    ld h, a
    ld (hl), e
    inc hl
    ld (hl), d
    inc hl
    ld a, l                     ; (ring: wrap at the end)
    cp a, #<(_bfs_q + 256)
    jr nz, 22$
    ld a, h
    cp a, #>(_bfs_q + 256)
    jr nz, 22$
    ld hl, #_bfs_q
22$:
    ld a, l
    ld (_bfs_tail), a
    ld a, h
    ld (_bfs_tail+1), a
    ld a, (_bfs_goal)           ; the NPC cell: remember its distance
    cp a, e
    jr nz, 21$
    ld a, (_bfs_goal+1)
    cp a, d
    jr nz, 21$
    ld a, c
    ld (_bfs_stop), a
21$:
    pop de
    ret
    __endasm;
}

/* One step of path finding per frame: start the next pending search, or continue one. */
static uint8_t field_take(uint8_t n)                  /* the NPC's field, or a free one */
{
    uint8_t f;
    if (npc[n].field != 0xFFu) return 1;
    for (f = 0; f < NFIELD; f++)
        if (field_owner[f] == 0xFFu) { field_owner[f] = n; npc[n].field = f; return 1; }
    return 0;
}
static void field_give(uint8_t n)
{
    if (npc[n].field == 0xFFu) return;
    if (bfs_cur == n) bfs_cur = 0xFF;
    field_owner[npc[n].field] = 0xFF;
    npc[n].field = 0xFF;
}

static void bfs_step(void)
{
    uint8_t k;
    if (bfs_cur == 0xFFu) {
        if ((LCDC_REG & LCDCF_ON) && LY_REG > 50u && LY_REG < 144u) return;   /* not enough of the frame left to start */
        for (k = 0; k < nnpc; k++)
            if ((bfs_pending & (1u << k)) && field_take(k)) { bfs_pending &= (uint8_t)~(1u << k); bfs_start(k); break; }
        if (bfs_cur == 0xFFu) return;
    }
    if (bfs_run()) { npc[bfs_cur].path_ok = 1; bfs_cur = 0xFF; }
}

void set_dest(uint8_t n, uint8_t x, uint8_t y)
{
    Npc *e = &npc[n];
    e->tx = x; e->ty = y;
    e->path_ok = 0; e->arrived = 0;
    bfs_pending |= (uint8_t)(1u << n);
    if (bfs_cur == n) bfs_cur = 0xFF;                  /* a search for the old destination: drop it */
}

/* ---- player ---- */
uint8_t feet_blocked(uint8_t x, uint8_t y)  /* feet box x-4..x+3, y-3..y+2 */
{
    if (x < 4u || x > SCR_W - 4u || y < 3u || y > PLAY_H - 3u) return 1;
    return cell_solid(x - 4u, y - 3u) || cell_solid(x + 3u, y - 3u)
        || cell_solid(x - 4u, y + 2u) || cell_solid(x + 3u, y + 2u);
}
static uint16_t psubx, psuby;
static void update_player(void)
{
    int8_t dx = 0, dy = 0;
    uint16_t sp;
    uint8_t n, slow = grab_n != 0xFFu || crouch;
    if (tie_n != 0xFFu) { pstep = 0; panim = 13; return; }   /* busy tying */
    if (keys & J_LEFT) dx--;
    if (keys & J_RIGHT) dx++;
    if (keys & J_UP) dy--;
    if (keys & J_DOWN) dy++;
    if (dx || dy) {
        pdir = DIR_FROM[dy + 1][dx + 1];
        if (++panim >= 14u) { panim = 0; pstep = (pstep + 1u) & 3u; }
    } else { pstep = 0; panim = 13; }
    sp = slow ? ((dx && dy) ? 91u : 128u) : ((dx && dy) ? PLAYER_DIAG : PLAYER_SPEED);   /* crouching, dragging: 0.5 */
    psubx = dx ? psubx + sp : 0;
    psuby = dy ? psuby + sp : 0;
    for (n = (uint8_t)(psubx >> 8), psubx &= 0xFFu; n; n--)
        if (!feet_blocked((uint8_t)(PX(pfx) + dx), PX(pfy))) pfx += (int16_t)dx << 8;
    for (n = (uint8_t)(psuby >> 8), psuby &= 0xFFu; n; n--)
        if (!feet_blocked(PX(pfx), (uint8_t)(PX(pfy) + dy))) pfy += (int16_t)dy << 8;
}

/* ---- NPCs ---- */
static uint8_t step_len(uint8_t op) { return (op == R_GO || op == R_FREE) ? 3u : (op == R_END ? 1u : 2u); }

static void npc_reset(void)
{
    uint8_t n, k;
    const NpcDef *d;
    Npc *e;
    memset(npc, 0, sizeof npc);
    memset(field_owner, 0xFF, sizeof field_owner);
    n_down = 0;
    nnpc = lv->nnpc;
    for (n = 0; n < NNPC; n++) npc[n].field = 0xFF;
    for (n = 0; n < nnpc; n++) {
        d = &lv->npc[n]; e = &npc[n];
        e->fx = (uint16_t)d->x << 8; e->fy = (uint16_t)d->y << 8;
        e->ang = e->want = d->face;
        e->face = d->face; e->sweep = d->sweep;
        e->nx = d->x; e->ny = d->y;
        e->home_x = d->x; e->home_y = d->y;
        e->rot_left = 0xFFFFu;
        e->route = d->route;
        if (e->route) {
            const uint8_t *p = e->route;
            for (k = 0; k < d->step; k++) p += step_len(*p);    /* step index -> byte offset */
            e->ri = (uint8_t)(p - e->route);
            for (p = e->route; *p != R_END; p += step_len(*p)) if (*p == R_WAIT) e->relay = 1;
            e->arrived = 1;                                     /* (the first step sets a destination) */
        } else {
            e->post_x = d->x; e->post_y = d->y;
            set_dest(n, d->x, d->y);
        }
    }
}

/* Walk towards the destination along the distance field. Returns 1 on arrival. */
uint8_t npc_walk(uint8_t n, uint16_t speed)
{
    Npc *e = &npc[n];
    uint8_t *d;
    int16_t ddx, ddy;
    uint16_t c, best_c, m, step;
    uint8_t k, best, x = PX(e->fx), y = PX(e->fy);

    if (e->arrived) return 1;
    if (!e->path_ok) return 0;
    d = field[e->field];
    ddx = (int16_t)((uint16_t)e->nx << 8) - (int16_t)e->fx;
    ddy = (int16_t)((uint16_t)e->ny << 8) - (int16_t)e->fy;
    if (!ddx && !ddy) {                                /* at the next point: choose the one after */
        c = ROWP[y >> 3] + (x >> 3);
        if (d[c] == 0u) {
            if (x == e->tx && y == e->ty) { e->arrived = 1; field_give(n); return 1; }
            e->nx = e->tx; e->ny = e->ty;
        } else {
            static const int8_t NB8[8] = { -1, 1, -(int8_t)PW, (int8_t)PW, -(int8_t)PW - 1, -(int8_t)PW + 1, (int8_t)PW - 1, (int8_t)PW + 1 };
            best = d[c]; best_c = c;
            for (k = 0; k < 8u; k++) {
                m = c + NB8[k];
                if (d[m] >= best) continue;
                if (k >= 4u) {                         /* diagonal: no cutting corners */
                    int8_t ox = (k & 1u) ? 1 : -1, oy = (k < 6u) ? -(int8_t)PW : (int8_t)PW;
                    if (solid[c + ox] || solid[c + oy]) continue;
                }
                best = d[m]; best_c = m;
            }
            if (best_c == c) { e->arrived = 1; field_give(n); return 1; }   /* unreachable: give up here */
            {
                int8_t o = (int8_t)(best_c - c), cx = 0, cy = 0;
                if (o == -1 || o == -(int8_t)PW - 1 || o == (int8_t)PW - 1) cx = -1;
                if (o == 1 || o == -(int8_t)PW + 1 || o == (int8_t)PW + 1) cx = 1;
                if (o <= -(int8_t)PW + 1) cy = -1;
                if (o >= (int8_t)PW - 1) cy = 1;
                e->nx = (uint8_t)(((x >> 3) + cx) * 8 + 4);
                e->ny = (uint8_t)(((y >> 3) + cy) * 8 + 4);
            }
        }
        ddx = (int16_t)((uint16_t)e->nx << 8) - (int16_t)e->fx;
        ddy = (int16_t)((uint16_t)e->ny << 8) - (int16_t)e->fy;
        if (!ddx && !ddy) return 0;
        e->want = atan2b(ddx, ddy);                    /* face along the new leg */
    }
    step = (ddx && ddy) ? (uint16_t)((speed * 181u) >> 8) : speed;
    if (ddx > 0) e->fx += (uint16_t)ddx < step ? (uint16_t)ddx : step;
    else if (ddx < 0) e->fx -= (uint16_t)-ddx < step ? (uint16_t)-ddx : step;
    if (ddy > 0) e->fy += (uint16_t)ddy < step ? (uint16_t)ddy : step;
    else if (ddy < 0) e->fy -= (uint16_t)-ddy < step ? (uint16_t)-ddy : step;
    if (++e->anim_t >= 8u) { e->anim_t = 0; e->step = (e->step + 1u) & 3u; }
    return 0;
}

void npc_turn(Npc *e)
{
    int8_t df = (int8_t)(e->want - e->ang);
    if (df > (int8_t)TURN_RATE) e->ang += TURN_RATE;
    else if (df < -(int8_t)TURN_RATE) e->ang -= TURN_RATE;
    else e->ang = e->want;
}

void kill_player(void)
{
    gstate = G_DEAD;
    banner("YOU WERE CAUGHT", "A: TRY AGAIN");
}

static void go_attack(uint8_t n)
{
    Npc *e = &npc[n];
    e->state = NS_ATTACK;
    e->lx = PX(pfx); e->ly = PX(pfy);
    set_dest(n, e->lx, e->ly);
    e->retarget_t = 4;
}

static void search_next(uint8_t n)
{
    Npc *e = &npc[n];
    uint8_t cx, cy, tries;
    for (tries = 0; tries < 8u; tries++) {
        cx = (uint8_t)((e->sx >> 3) + (rnd() % (2u * SEARCH_RANGE + 1u)) - SEARCH_RANGE);
        cy = (uint8_t)((e->sy >> 3) + (rnd() % (2u * SEARCH_RANGE + 1u)) - SEARCH_RANGE);
        if (cx < CELLS_W && cy < CELLS_H && !solid[PIDX(cx, cy)]) break;
    }
    if (tries == 8u) { cx = e->sx >> 3; cy = e->sy >> 3; }
    set_dest(n, (uint8_t)(cx * 8u + 4u), (uint8_t)(cy * 8u + 4u));
}

void go_suspicious(uint8_t n, uint8_t x, uint8_t y)
{
    Npc *e = &npc[n];
    e->state = NS_SUSPICIOUS;
    e->state_t = SEARCH_FRAMES;
    e->sx = x; e->sy = y;
    e->pause_t = 10;                                   /* a moment of "huh?" first */
    search_next(n);
}

static int8_t turn_sign(const uint8_t *s) { return *s == R_TURN || *s == R_CW ? 1 : -1; }

/* A route NPC picks up its script again. A rotation guard must keep its place in the loop:
 * it walks back to where it stood, and a turn it was in the middle of ends at the same facing. */
static void resume_route(uint8_t n)
{
    Npc *e = &npc[n];
    const uint8_t *s = e->route + e->ri;
    e->route_dest = 0;
    if (!e->relay) { e->rot_left = 0xFFFFu; return; }
    if (*s != R_GO) { e->returning = 1; set_dest(n, e->home_x, e->home_y); }
    if (e->rot_left != 0xFFFFu) e->rot_left = (uint8_t)((uint8_t)(e->turn_end - e->ang) * (uint8_t)turn_sign(s));
}

/* Back from searching: guards go back to their post, route NPCs to the nearest point of
 * their route (rotation guards to where they were). */
static void back_to_duty(uint8_t n)
{
    Npc *e = &npc[n];
    const uint8_t *p;
    uint8_t x = PX(e->fx), y = PX(e->fy);
    uint16_t bd = 0xFFFFu, dd;
    e->state = NS_NORMAL; e->spot = 0;
    if (!e->route) { set_dest(n, e->post_x, e->post_y); return; }
    if (e->relay) { resume_route(n); return; }
    for (p = e->route; *p != R_END; p += step_len(*p)) {
        if (*p != R_GO) continue;
        dd = (uint16_t)ad(x, p[1]) + ad(y, p[2]);
        if (dd < bd) { bd = dd; e->ri = (uint8_t)(p - e->route); }
    }
    e->route_dest = 0; e->rot_left = 0xFFFFu;
}

/* Back to duty from a coin errand: carry on with the current step. */
void resume_duty(uint8_t n)
{
    Npc *e = &npc[n];
    e->state = NS_NORMAL;
    if (!e->route) set_dest(n, e->post_x, e->post_y);
    else resume_route(n);
}
static void coin_off(Npc *e)                            /* off a coin errand: someone else may have it */
{
    if (e->state == NS_COIN && e->c_item < NITEMS && items[e->c_item].kind == IT_COIN) items[e->c_item].flags &= (uint8_t)~IF_CLAIMED;
    e->c_item = 0xFF;
}

static uint8_t spot_free(uint8_t n, uint8_t x, uint8_t y)   /* nobody else stands at (x, y) */
{
    uint8_t k;
    for (k = 0; k < nnpc; k++)
        if (k != n && ad(PX(npc[k].fx), x) < 8u && ad(PX(npc[k].fy), y) < 8u) return 0;
    return 1;
}

/* The NPC's normal duty, one frame. Returns 1 while a route NPC is walking (it looks around then). */
static uint8_t npc_duty(uint8_t n)
{
    Npc *e = &npc[n];
    const uint8_t *s;
    uint8_t r;
    if (e->route) {
        if (e->returning) {                            /* back to where it stood before */
            if (!npc_walk(n, PATROL_SPEED)) return 1;
            e->returning = 0;
        }
        s = e->route + e->ri;
        switch (*s) {
        case R_END:
            e->ri = 0; e->route_dest = 0; return 0;
        case R_GO:
            if (!e->route_dest) { set_dest(n, s[1], s[2]); e->route_dest = 1; }
            if (!npc_walk(n, PATROL_SPEED)) return 1;
            e->home_x = s[1]; e->home_y = s[2];
            break;
        case R_TURN: case R_CW: case R_CCW:            /* turning on the spot */
            if (e->rot_left == 0xFFFFu) {
                if (*s == R_TURN) { e->rot_left = s[1] ? s[1] : 256u; e->turn_end = (uint8_t)(e->ang + s[1]); }
                else { e->turn_end = s[1]; e->rot_left = (uint8_t)((uint8_t)(s[1] - e->ang) * (uint8_t)turn_sign(s)); }
            }
            r = e->rot_left < TURN_RATE ? (uint8_t)e->rot_left : TURN_RATE;
            e->ang = turn_sign(s) > 0 ? (uint8_t)(e->ang + r) : (uint8_t)(e->ang - r);
            e->want = e->ang; e->rot_left -= r;
            if (e->rot_left) return 0;
            e->rot_left = 0xFFFFu;
            break;
        case R_TO:                                     /* turn the short way to a facing */
            e->want = s[1]; npc_turn(e);
            if (e->ang != s[1]) return 0;
            break;
        case R_SIG:                                    /* bump: the pair there starts its shift */
            signals[s[1]]++;
            break;
        case R_WAIT:                                   /* stand at the post until the next bump */
            if (!e->has_wait) { e->has_wait = 1; e->wait_base = signals[s[1]]; }
            if (signals[s[1]] == e->wait_base) return 0;
            e->has_wait = 0;
            break;
        case R_FREE:                                   /* wait until the post is empty */
            if (!spot_free(n, s[1], s[2])) return 0;
            break;
        }
        e->ri += step_len(*s);
        e->route_dest = 0;
        return 0;
    }
    if (!npc_walk(n, PATROL_SPEED)) return 0;          /* guard: back to the post first */
    if (e->look_t) { e->look_t--; return 0; }          /* at the post: sweep, pausing at each end */
    if (e->want == e->ang) {
        uint8_t a = (uint8_t)(e->face - e->sweep), b = (uint8_t)(e->face + e->sweep);
        e->want = e->want == a ? b : a;
        e->look_t = 45;
    }
    return 0;
}

void knock_out(uint8_t n)
{
    Npc *e = &npc[n];
    coin_off(e); e->d_on = 0;
    e->state = NS_DOWN; e->seen = 0; e->state_t = KO_FRAMES; e->look = 0; e->spot = 0;
    n_down++;
    bfs_pending &= (uint8_t)~(1u << n);
    field_give(n);
}

/* Approximate distance (max + 3/8 min). */
uint8_t dist8(uint8_t a, uint8_t b) { return a > b ? (uint8_t)(a + ((b * 3u) >> 3)) : (uint8_t)(b + ((a * 3u) >> 3)); }

static uint8_t sees_body(uint8_t n)                    /* a knocked-out NPC in the cone */
{
    uint8_t k;
    if (!n_down) return 0;
    Npc *e = &npc[n];
    for (k = 0; k < nnpc; k++) {
        Npc *o = &npc[k];
        if (k == n || o->state != NS_DOWN) continue;
        if (in_bush(PX(o->fx), (uint8_t)(PX(o->fy) - 6u + LIE_DROP))) continue;   /* hidden in a bush */
        if (cone_sees_box(n, (uint8_t)(PX(o->fx) - 6u), (uint8_t)(PX(o->fy) - 9u + LIE_DROP), 12u, 8u)) {
            coin_off(e); e->d_on = 0;
            go_suspicious(n, PX(o->fx), PX(o->fy));
            return 1;
        }
    }
    return 0;
}

static void update_npc(uint8_t n)
{
    Npc *e = &npc[n];
    uint8_t x = PX(e->fx), y = PX(e->fy), px = PX(pfx), py = PX(pfy);

    if (e->state == NS_DOWN) {                         /* back on its feet after 20 s, suspicious */
        if (e->tied || --e->state_t) return;           /* (tied up: down for good) */
        n_down--;
        if (grab_n == n) grab_n = 0xFF;
        if (tie_n == n) { tie_n = 0xFF; e->state = NS_ATTACK; kill_player(); return; }   /* woke up while being tied */
        go_suspicious(n, x, y);
        return;
    }
    if (e->state == NS_GONE) return;
    /* bumped into it (a calm NPC walks past you crouching in a bush) */
    if (ad(x, px) < TOUCH_X && ad(y, py) < TOUCH_Y && !(crouch && (e->state == NS_NORMAL || e->state == NS_COIN))) { kill_player(); return; }
    if (n_armed) {                                     /* stepped in a trap */
        Item *it = items;
        for (px = NITEMS; px; px--, it++)
            if ((it->flags & IF_ARMED) && (uint8_t)(it->x + 11u - x) < 15u && (uint8_t)(it->y + 10u - y) < 13u) {
                it->flags &= (uint8_t)~IF_ARMED; n_armed--; knock_out(n); return;
            }
        px = PX(pfx);
    }
    if (e->state != NS_ATTACK) {
        if (e->seen) {                                 /* being spotted: stop and stare */
            if (e->state == NS_COIN) { coin_off(e); resume_duty(n); }   /* seeing you beats the coin */
            e->d_on = 0;
            uint8_t d = dist8(ad(px, x), ad((uint8_t)(py - 6u), (uint8_t)(y - 5u)));
            if (d > CONE_LEN) d = CONE_LEN;
            if (d < 1u) d = 1u;
            e->spot += (uint16_t)(1920u / d);          /* full after 2.25 s at 60 px, faster closer */
            if (e->spot >= SPOT_FULL) { go_attack(n); return; }
            e->lx = px; e->ly = py;
            npc_turn(e);
            return;
        }
        if (e->spot) {
            e->spot = e->spot > SPOT_DECAY ? e->spot - SPOT_DECAY : 0;
            if (e->state == NS_NORMAL) go_suspicious(n, e->lx, e->ly);
        }
    }
    if (e->d_on && e->state != NS_ATTACK) { npc_distract(n); return; }   /* looking at where a stone landed */
    if ((e->state == NS_NORMAL || e->state == NS_COIN) && sees_body(n)) return;
    if (e->state == NS_COIN) { npc_coin(n); return; }
    if (e->state == NS_NORMAL && n_thrown && npc_see_coin(n)) return;   /* a coin in view: go and get it */

    switch (e->state) {
    case NS_NORMAL:
        if (npc_duty(n)) {                             /* look left and right while walking */
            e->sway += SWAY_STEP;
            e->look = SWAY_T[e->sway >> 11];
            npc_turn(e);
            return;
        }
        break;
    case NS_SUSPICIOUS:
        if (!--e->state_t) { back_to_duty(n); break; }
        if (e->pause_t) {                              /* look around */
            e->pause_t--;
            if (!(e->pause_t & 15u)) e->want = rnd();
            break;
        }
        if (npc_walk(n, SEARCH_SPEED)) { e->pause_t = 32; search_next(n); }
        break;
    case NS_ATTACK:
        if (ad(x, px) <= KILL_DIST && ad(y, py) <= KILL_DIST) { kill_player(); break; }
        if (e->seen) {                                 /* still in sight: follow */
            e->lx = px; e->ly = py;
            if (e->retarget_t) e->retarget_t--;
            else if ((e->lx >> 3) != (e->tx >> 3) || (e->ly >> 3) != (e->ty >> 3)) {
                set_dest(n, e->lx, e->ly); e->retarget_t = 4;
            }
        }
        if (npc_walk(n, ATTACK_SPEED)) {
            /* at the last known position and the player isn't here: look again */
            uint8_t ex = x, ey = (uint8_t)(y - 5u), cx = px, cy = (uint8_t)(py - 6u);
            if (dist8(ad(ex, cx), ad(ey, cy)) <= LOOK_RADIUS && los(ex, ey, cx, cy)) {
                e->lx = px; e->ly = py; set_dest(n, px, py);
            } else go_suspicious(n, x, y);
        }
        break;
    }
    if (e->look) {                                     /* otherwise look straight ahead */
        if (e->look > 0) e->look -= (int8_t)((e->look + 7) >> 3);
        else e->look += (int8_t)((-e->look + 7) >> 3);
    }
    npc_turn(e);
}

static void level_done(const char *text)
{
    gstate = G_CLEAR;
    banner(text, level_no + 1u < NUM_LEVELS ? "A: NEXT LEVEL" : "A: BACK TO START");
}

void check_clear(void)
{
    uint8_t n, px = PX(pfx), py = PX(pfy);
    const Box *x = &lv->exit;
    if (gstate != G_PLAY) return;
    if (lv->flags & LF_ALLDOWN) {
        for (n = 0; n < nnpc && (npc[n].state == NS_DOWN || npc[n].state == NS_GONE); n++) ;
        if (n == nnpc) { level_done("AREA CLEAR!"); return; }
    }
    if (!(lv->flags & LF_LOCKED) && px >= x->x && px < (uint8_t)(x->x + x->w) && (uint8_t)(py - 4u) < (uint8_t)(x->y + x->h))
        level_done("LEVEL CLEAR!");
}

void try_takedown(void)
{
    uint8_t n, px = PX(pfx), py = PX(pfy);
    for (n = 0; n < nnpc; n++) {
        Npc *e = &npc[n];
        if (e->state == NS_DOWN || e->state == NS_GONE || e->state == NS_ATTACK) continue;
        if (ad(PX(e->fx), px) <= TAKEDOWN_DIST && ad(PX(e->fy), py) <= TAKEDOWN_DIST) {
            if (e->state == NS_SUSPICIOUS && e->seen) { kill_player(); return; }   /* it fights back */
            knock_out(n);
        }
    }
    check_clear();
}

/* ---- sprites ---- */
#define OAM_PLAYER 0u
#define OAM_NPC    2u           /* 2 per NPC */
#define OAM_ICON   12u          /* 1 per NPC: ? ! or the knock-out bar */
#define OAM_EXIT   17u          /* 2 */
static void hide(uint8_t n) { shadow_OAM[n].y = 0; }

static const uint8_t *person_frame(uint8_t row, uint8_t step) { return &hero_anim[row][step]; }

/* All sprites, written straight into the shadow OAM. */
static void draw_sprites(void)
{
    uint8_t n, x, y, t, *o, *ic;
    Npc *e;
    const Box *bx = &lv->exit;
    o = (uint8_t *)&shadow_OAM[OAM_PLAYER];
    x = PX(pfx); y = (uint8_t)(PX(pfy) + 3u);          /* (sprite y + 16 - 13) */
    if (gstate == G_DEAD) { t = T_LYING; y += LIE_DROP; }
    else if (crouch) t = T_GHOST;                      /* crouching in a bush: see-through */
    else t = (uint8_t)(*person_frame(pdir, pstep) << 2);
    o[0] = y; o[1] = x; o[2] = t; o[4] = y; o[5] = (uint8_t)(x + 8u); o[6] = (uint8_t)(t + 2u);
    o = (uint8_t *)&shadow_OAM[OAM_NPC];
    ic = (uint8_t *)&shadow_OAM[OAM_ICON];
    for (n = 0, e = npc; n < NNPC; n++, e++, o += 8, ic += 4) {
        if (n >= nnpc) { o[0] = 0; o[4] = 0; ic[0] = 0; continue; }
        if (((n ^ (uint8_t)frame) & 1u) && gstate == G_PLAY && frame > 2u && n != grab_n) continue;   /* moved last frame: drawn then */
        if (e->state == NS_GONE) { o[0] = 0; o[4] = 0; ic[0] = 0; continue; }
        x = PX(e->fx); y = (uint8_t)(PX(e->fy) + 3u);
        if (e->state == NS_DOWN) {
            y += LIE_DROP;
            if (in_bush(x, (uint8_t)(PX(e->fy) - 6u + LIE_DROP))) {   /* in a bush: see-through over the leaves */
                t = (uint8_t)(T_GHOST + 4u); o[3] = o[7] = 0;
                if ((e->vanish_t >> 2) & 1u) y = 0;    /* a tied body flickers out */
            } else { t = T_LYING; o[3] = o[7] = S_PALETTE; }
            o[0] = y; o[1] = x; o[2] = t; o[4] = y; o[5] = (uint8_t)(x + 8u); o[6] = (uint8_t)(t + 2u);
            /* time left until it wakes up */
            t = e->tied ? 6u : (uint8_t)(((e->state_t + 99u) * 41u) >> 12);   /* ceil(state_t / 100): 0..6 */
            ic[0] = (uint8_t)(y - 2u); ic[1] = (uint8_t)(x + 4u); ic[2] = (uint8_t)(T_BAR + t * 2u); ic[3] = S_PALETTE;
            continue;
        }
        t = (uint8_t)(*person_frame(ROW_FROM_OCT[(uint8_t)(e->ang + 16u) >> 5], e->step) << 2);
        o[3] = o[7] = near_bush(x, PX(e->fy)) ? (S_PALETTE | 0x80u) : S_PALETTE;   /* behind the leaves */
        o[0] = y; o[1] = x; o[2] = t; o[4] = y; o[5] = (uint8_t)(x + 8u); o[6] = (uint8_t)(t + 2u);
        ic[3] = 0;
        if (e->state == NS_ATTACK) t = T_BANG;
        else if (e->state == NS_SUSPICIOUS || (e->spot && (e->seen ? (frame & 4u) : 1u))) t = T_QUESTION;
        else { ic[0] = 0; continue; }
        ic[0] = (uint8_t)(y - 11u); ic[1] = (uint8_t)(x + 4u); ic[2] = t;
    }
    t = (lv->flags & LF_LOCKED) ? (uint8_t)(T_EXIT + 8u) : (uint8_t)(T_EXIT + (((uint8_t)frame >> 4) & 1u) * 4u);
    o = (uint8_t *)&shadow_OAM[OAM_EXIT];
    x = (uint8_t)(bx->x + (bx->w >> 1));
    o[0] = (uint8_t)(bx->y + 16u); o[1] = x; o[2] = t;
    o[4] = o[0]; o[5] = (uint8_t)(x + 8u); o[6] = (uint8_t)(t + 2u);
}

/* ---- cones ---- */
static uint8_t look_n;
static uint8_t c_on_[NCONES], c_ex_[NCONES], c_ey_[NCONES], c_k_[NCONES];
static void cones_work(void)                           /* uses the rest of the frame */
{
    cones_update(c_on_, c_ex_, c_ey_, c_k_, los_open ? CONE_LY : CONE_LY_WALLS);
}
static void draw_cones(void)
{
    uint8_t n, *on = c_on_, *ex = c_ex_, *ey = c_ey_, *k = c_k_;
    uint8_t bx = (uint8_t)(PX(pfx) - 4u), by = (uint8_t)(PX(pfy) - 12u);
    Npc *e;
    for (n = (uint8_t)frame & 1u, e = &npc[n]; n < NCONES; n += 2u, e += 2) {   /* the NPCs updated this frame */
        if (n >= nnpc || e->state == NS_DOWN || e->state == NS_GONE) { on[n] = 0; continue; }
        on[n] = 1;
        ex[n] = PX(e->fx);
        ey[n] = (uint8_t)(PX(e->fy) - 5u);
        k[n] = (uint8_t)((uint8_t)(e->ang + (uint8_t)(e->look >> 2) + 2u) >> 2);   /* 64 angles */
    }
    PROF_MARK(1);
    /* one NPC looks per frame, in turn; its "seen" stays until it looks again */
    if (nnpc) {
        if (++look_n >= nnpc) look_n = 0;
        e = &npc[look_n];
        /* crouching in a bush: only attackers see you */
        e->seen = gstate == G_PLAY && e->state != NS_DOWN && e->state != NS_GONE && !(crouch && e->state != NS_ATTACK) && cone_sees(look_n, bx, by);
    }
    PROF_MARK(2);
}

/* ---- setup ---- */
static void start_level(uint8_t i)
{
    uint8_t k;
    DISPLAY_OFF;
    level_no = i; lv = &levels[i];
    build_map();
    draw_map();
    vram_reset();
    cone_reset();
    for (k = 0; k < 40u; k++) hide(k);
    pfx = (uint16_t)lv->sx << 8; pfy = (uint16_t)lv->sy << 8;
    pdir = DIR_FROM[0][1]; pstep = 0; psubx = psuby = 0;
    signals[0] = signals[1] = 0;
    bfs_pending = 0; bfs_cur = 0xFF;
    npc_reset();
    items_reset();                                     /* (bank 3) */
    while (bfs_pending || bfs_cur != 0xFFu) bfs_step();   /* LCD is off: runs to the end */
    text_row(level_label(i));
    set_win_tiles(0, 0, 20, 1, rowbuf);
    WY_REG = PLAY_H;
    gstate = G_PLAY;
    frame = 0;
    SHOW_BKG; SHOW_WIN; SHOW_SPRITES;
    DISPLAY_ON;
}

static void title_level(void)                          /* level number and name, arrows bobbing */
{
    static char lvl[] = "LEVEL 1";
    uint8_t b = (frame >> 4) & 1u;
    lvl[6] = (char)('1' + sel_level);
    text_row(lvl);
    rowbuf[(uint8_t)(3u - b)] = B_ARROW;
    rowbuf[(uint8_t)(16u + b)] = B_ARROW + 1u;
    set_bkg_tiles(0, 7, 20, 1, rowbuf);
    text_row(levels[sel_level].name);
    set_bkg_tiles(0, 9, 20, 1, rowbuf);
}

static void show_title(void)
{
    static const char *const T[CELLS_H] = {
        "", "", "STEALTH", "", "SELECT LEVEL", "", "", "", "", "", "",
        "START: PLAY", "D-PAD: MOVE", "A: ACTION B: CROUCH", "SELECT: LEVELS", "", "ART: GIBBONGL CC0" };
    uint8_t y, k;
    DISPLAY_OFF;
    for (k = 0; k < 40u; k++) hide(k);
    for (y = 0; y < CELLS_H; y++) { text_row(T[y]); set_bkg_tiles(0, y, 20, 1, rowbuf); }
    text_row(""); set_bkg_tiles(0, 17, 20, 1, rowbuf);
    WY_REG = SCR_H;
    gstate = G_TITLE;
    title_level();
    SHOW_BKG; SHOW_SPRITES;
    DISPLAY_ON;
}

void main(void)
{
    uint8_t k, vbl;
    DISPLAY_OFF;
    LCDC_REG = (uint8_t)((LCDC_REG & (uint8_t)~LCDCF_BG8000) | LCDCF_WIN9C00 | LCDCF_BG9800 | LCDCF_OBJ16);
    load_tiles();                                      /* (bank 2) */
    BGP_REG = 0xE4u;           /* 0 white, 1 light grey (cones), 2 dark grey, 3 black */
    OBP0_REG = 0xD0u;          /* player and icons: 1 = white, 2 = light grey, 3 = black */
    OBP1_REG = 0xE4u;          /* NPCs: the same sprites one shade darker */
    WX_REG = 7; WY_REG = SCR_H;
    SPRITES_8x16;
    for (k = OAM_NPC; k < OAM_NPC + 2u * NNPC; k++) shadow_OAM[k].prop = S_PALETTE;
    sel_level = 0;
    show_title();
    vbl = (uint8_t)sys_time;

    while (1) {
        wait_vbl_done();
        if ((uint8_t)((uint8_t)sys_time - vbl) > 1u && gstate == G_PLAY) lag_frames++;
#ifdef PROF
        prof_log[prof_li & 63u] = (uint8_t)sys_time; prof_li++;
        prof_log[prof_li & 63u] = LY_REG; prof_li++;
#endif
        vbl = (uint8_t)sys_time;
        vram_flush();
        prev_keys = keys;
        keys = joypad();
        k = keys & (uint8_t)~prev_keys;                /* pressed this frame */
        frame++;
        if (gstate == G_TITLE) {
            rnd();
            if (k & J_LEFT) sel_level = (uint8_t)((sel_level + NUM_LEVELS - 1u) % NUM_LEVELS);
            if (k & (J_RIGHT | J_SELECT)) sel_level = (uint8_t)((sel_level + 1u) % NUM_LEVELS);
            if (k & (J_START | J_A)) { start_level(sel_level); continue; }
            title_level();
            continue;
        }
        if (k & J_SELECT) { sel_level = level_no; show_title(); continue; }
#ifdef PROF
        PROF_MARK(7);
#endif
        if (gstate == G_PLAY) {
            if (!aiming) update_player();
            items_player(k);                           /* (bank 3) items, A, B, dragging, tying */
            check_clear();
            bfs_step();                                /* (early in the frame: path finding starts only then) */
        } else if (k & J_A) {
            if (gstate == G_DEAD) start_level(level_no);
            else if (level_no + 1u < NUM_LEVELS) { sel_level = level_no + 1u; start_level(sel_level); }
            else { sel_level = 0; show_title(); }
            continue;
        }
        PROF_MARK(0);
        draw_cones();
        if (gstate == G_PLAY)
            for (k = frame & 1u; k < nnpc && gstate == G_PLAY; k += 2u) update_npc(k);   /* half per frame */
        PROF_MARK(3);
        draw_sprites();
        items_draw();
        if (gstate == G_PLAY) { if (frame >= 120u) items_bar(frame == 120u); }   /* (the level name first) */
        else for (k = 0; k < 40u; k++) if (shadow_OAM[k].y >= 113u) hide(k);   /* nothing over the banner */
        PROF_MARK(4);
        cones_work();
        PROF_MARK(8);
#ifdef PROF
        prof_log[prof_li & 63u] = LY_REG; prof_li++;
        { uint8_t l = LY_REG, f = l >= 144u ? (uint8_t)(l - 144u) : (uint8_t)(l + 10u); if (f > 145u || f < 8u) { prof_over++; if (f < 8u) memcpy(prof_worst, prof_pos, 20); } if (f < 8u) prof_wrap++; if (f > prof_max) prof_max = f; }
#endif
        PROF_MARK(6);                                  /* (VRAM writes go out in vblank: cones only change map bytes) */
    }
}
