/*
 * main.c - Tether: a deep-sea diver on an air hose. Game Boy (DMG), GBDK-2020.
 *
 * Collect every gem, then swim back to the start pump to be lifted out.
 * The air hose runs straight from the pump and catches on rocks. Touch
 * another pump to switch: the hose reels in to the diver (costs air), then
 * attaches to the new pump. Fish bite the hose: first they wear down its
 * HP, then every bite makes a hole that leaks 1 air per second. Air never
 * refills; at 0 the diver drowns.
 *
 * Controls: D-pad swim. START / A: start, retry, continue.
 * Title screen: LEFT / RIGHT picks the level.
 *
 * Credits: hero sprites "8-Directional Game Boy Character Template" by
 * GibbonGL (CC0), https://gibbongl.itch.io/8-directional-gameboy-character-template
 */
#include <gb/gb.h>
#include <gb/hardware.h>
#include <stdint.h>
#include <string.h>
#include "game.h"
#include "gfx.h"


/* ------------------------------------------------------------------ */
/* Tuning (all "adjust later")                                         */
/* ------------------------------------------------------------------ */
#define AIR_MAX        6000u    /* 100 air, stored x60 so drains are whole numbers per frame */
#define REEL_SPEED     2        /* px of hose reeled in per frame */
#define REEL_DRAIN     4u       /* air x60 per frame while reeling = 4 air per second */
#define HOLE_DRAIN     1u       /* per hole, air x60 per frame = 1 air per second */
#define HOSE_HP        3
#define BREATH_EVERY   180u     /* breathing bubble every 3 s */
#define GLIMMER_EVERY  300u     /* each gem glimmers every 5 s, gems take turns */
#define GLIMMER_LEN    24u
#define SPOT_DIST2     16       /* fish stops when its mouth is 4 px from the hose */
#define WINDUP         18       /* pause before the jump, 0.3 s */
#define HOLD_FRAMES    120      /* fish hangs on 2 s */
#define FLAP_FRAMES    15       /* tail flap: 2 per second */
#define GRIP_DIST2     9        /* lets go if the hose moves more than 3 px away */
#define BITE_COOLDOWN  60
#define HOOK_SPEED     3
#define MSG_FRAMES     180u

#define SPEED_STRAIGHT 256u     /* 1 px per frame, 8.8 fixed point */
#define SPEED_DIAG     181u     /* 1/sqrt(2) per axis */

/* OAM slots */
#define OAM_DIVER   0u   /* 2 */
#define OAM_FISH    2u   /* 6 */
#define OAM_GEM     8u   /* 6 */
#define OAM_ARROW   14u
#define OAM_HOOK    15u
#define OAM_LINE    16u  /* 9 */
#define OAM_BUBBLE  25u  /* 10 */
#define MAX_BUBBLES 10u
#define NUM_OAM     35u

enum { ST_TITLE, ST_INTRO, ST_PLAY, ST_OUTRO, ST_DONE, ST_DEAD, ST_END };
enum { SUB_LOWER, SUB_HOOKUP, SUB_REEL, SUB_HOOKDOWN, SUB_PAUSE, SUB_LIFT };
enum { F_SWIM, F_STOP, F_JUMP, F_HOLD };

#define DIR_DOWN  0u
#define DIR_UP    4u
static const uint8_t DIR_FROM[3][3] = {   /* [dy+1][dx+1] -> sheet row */
    { 3, 4, 5 },
    { 2, 0, 6 },
    { 1, 0, 7 }
};
static const int8_t SWAY[8] = { 0, 1, 1, 0, 0, -1, -1, 0 };

/* ------------------------------------------------------------------ */
/* State. Global rather than static, so the variables appear in the linker
 * map (tether.map) and can be watched in an emulator's memory viewer. */
/* ------------------------------------------------------------------ */
uint8_t keys, prev_keys, state, sub, lv, sel_level;
uint16_t pt, frame, rng;

uint8_t npumps, nrocks, ngems, nfish;
uint8_t pumpx[MAX_PUMPS], pumpy[MAX_PUMPS];
uint8_t rockx[MAX_ROCKS], rocky[MAX_ROCKS];
uint8_t gemx[MAX_GEMS], gemy[MAX_GEMS], gemgot[MAX_GEMS];
uint8_t ngot, cleared;

int16_t fx16[MAX_FISH];               /* fish x in 1/16 px */
uint8_t fy[MAX_FISH];
int8_t  fv[MAX_FISH];
uint8_t fst[MAX_FISH], ft[MAX_FISH], fcd[MAX_FISH];
int8_t  fjx[MAX_FISH], fjy[MAX_FISH];
uint8_t fflap[MAX_FISH], fflap_t[MAX_FISH];

int16_t hx, hy;                       /* diver top-left */
uint8_t dir, step, anim_t, start_x, start_y;
uint16_t subx, suby;
int16_t hook_y, lift_x;

uint8_t chx[MAX_PIV], chy[MAX_PIV], chlen[MAX_PIV], nch;
int8_t  chside[MAX_PIV];
uint16_t fixed_len;
uint8_t hose_len, pump, hose_on, hose_dirty, fixed_dirty;

uint8_t reeling, reel_free;
int8_t  reel_to;
uint8_t rx[MAX_PIV], ry[MAX_PIV], rlen[MAX_PIV], nr;
uint16_t reel_eaten;

uint16_t air;
uint8_t hp, nholes;
uint16_t holes[MAX_HOLES];

int16_t bx16[MAX_BUBBLES], by16[MAX_BUBBLES];
int8_t  bvx[MAX_BUBBLES];
uint8_t bvy[MAX_BUBBLES], bt[MAX_BUBBLES], blife[MAX_BUBBLES], bwob[MAX_BUBBLES], bkind[MAX_BUBBLES];
static uint8_t gem_tile[MAX_GEMS], bub_shown[MAX_BUBBLES];   /* last sprite state, to skip rewrites */

uint8_t msg_t, banner_rows, hud_cache[20], pump_light;
uint16_t glim_t;
uint8_t hose_k;                  /* 40 * 256 / hose length, for the HUD bar */
typedef struct {
    int16_t x, y;       /* rock - pivot */
    int16_t c;          /* cross(diver - pivot, rock - pivot) */
    uint8_t act;        /* rock within hose reach of the pivot */
} RockTrack;
static RockTrack rt[MAX_ROCKS];
static uint8_t rcr_dirty = 1;
uint8_t breath_t, hole_t[MAX_HOLES], hud_t, hose_phase;
uint16_t gem_phase[MAX_GEMS];

/* ------------------------------------------------------------------ */
/* Small maths                                                         */
/* ------------------------------------------------------------------ */

/* Squares 0..255: the Game Boy CPU has no multiply, so a table is much faster. */
static const uint16_t SQ[256] = { 0,1,4,9,16,25,36,49,64,81,100,121,144,169,196,225,256,289,324,361,400,441,484,529,576,625,676,729,784,841,900,961,1024,1089,1156,1225,1296,1369,1444,1521,1600,1681,1764,1849,1936,2025,2116,2209,2304,2401,2500,2601,2704,2809,2916,3025,3136,3249,3364,3481,3600,3721,3844,3969,4096,4225,4356,4489,4624,4761,4900,5041,5184,5329,5476,5625,5776,5929,6084,6241,6400,6561,6724,6889,7056,7225,7396,7569,7744,7921,8100,8281,8464,8649,8836,9025,9216,9409,9604,9801,10000,10201,10404,10609,10816,11025,11236,11449,11664,11881,12100,12321,12544,12769,12996,13225,13456,13689,13924,14161,14400,14641,14884,15129,15376,15625,15876,16129,16384,16641,16900,17161,17424,17689,17956,18225,18496,18769,19044,19321,19600,19881,20164,20449,20736,21025,21316,21609,21904,22201,22500,22801,23104,23409,23716,24025,24336,24649,24964,25281,25600,25921,26244,26569,26896,27225,27556,27889,28224,28561,28900,29241,29584,29929,30276,30625,30976,31329,31684,32041,32400,32761,33124,33489,33856,34225,34596,34969,35344,35721,36100,36481,36864,37249,37636,38025,38416,38809,39204,39601,40000,40401,40804,41209,41616,42025,42436,42849,43264,43681,44100,44521,44944,45369,45796,46225,46656,47089,47524,47961,48400,48841,49284,49729,50176,50625,51076,51529,51984,52441,52900,53361,53824,54289,54756,55225,55696,56169,56644,57121,57600,58081,58564,59049,59536,60025,60516,61009,61504,62001,62500,63001,63504,64009,64516,65025 };

/* Integer square root by binary search in the squares table (8 steps, no multiply). */
static uint8_t isqrt16(uint16_t v)
{
    uint8_t lo = 0, bit = 0x80, t;
    while (bit) {
        t = lo | bit;
        if (SQ[t] <= v) lo = t;
        bit >>= 1;
    }
    return lo;
}

static uint16_t d2(int16_t dx, int16_t dy)
{
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    if (dx > 255) dx = 255;
    if (dy > 255) dy = 255;
    return SQ[(uint8_t)dx] + SQ[(uint8_t)dy];
}

/* Multiply by quarter squares: a*b = floor((a+b)^2/4) - floor((a-b)^2/4).
 * The Game Boy CPU has no multiply instruction, so a table lookup is much faster.
 * Valid for |a|, |b| <= 160 (everything on screen). */
static const uint16_t QSQ[321] = { 0,0,1,2,4,6,9,12,16,20,25,30,36,42,49,56,64,72,81,90,100,110,121,132,144,156,169,182,196,210,225,240,256,272,289,306,324,342,361,380,400,420,441,462,484,506,529,552,576,600,625,650,676,702,729,756,784,812,841,870,900,930,961,992,1024,1056,1089,1122,1156,1190,1225,1260,1296,1332,1369,1406,1444,1482,1521,1560,1600,1640,1681,1722,1764,1806,1849,1892,1936,1980,2025,2070,2116,2162,2209,2256,2304,2352,2401,2450,2500,2550,2601,2652,2704,2756,2809,2862,2916,2970,3025,3080,3136,3192,3249,3306,3364,3422,3481,3540,3600,3660,3721,3782,3844,3906,3969,4032,4096,4160,4225,4290,4356,4422,4489,4556,4624,4692,4761,4830,4900,4970,5041,5112,5184,5256,5329,5402,5476,5550,5625,5700,5776,5852,5929,6006,6084,6162,6241,6320,6400,6480,6561,6642,6724,6806,6889,6972,7056,7140,7225,7310,7396,7482,7569,7656,7744,7832,7921,8010,8100,8190,8281,8372,8464,8556,8649,8742,8836,8930,9025,9120,9216,9312,9409,9506,9604,9702,9801,9900,10000,10100,10201,10302,10404,10506,10609,10712,10816,10920,11025,11130,11236,11342,11449,11556,11664,11772,11881,11990,12100,12210,12321,12432,12544,12656,12769,12882,12996,13110,13225,13340,13456,13572,13689,13806,13924,14042,14161,14280,14400,14520,14641,14762,14884,15006,15129,15252,15376,15500,15625,15750,15876,16002,16129,16256,16384,16512,16641,16770,16900,17030,17161,17292,17424,17556,17689,17822,17956,18090,18225,18360,18496,18632,18769,18906,19044,19182,19321,19460,19600,19740,19881,20022,20164,20306,20449,20592,20736,20880,21025,21170,21316,21462,21609,21756,21904,22052,22201,22350,22500,22650,22801,22952,23104,23256,23409,23562,23716,23870,24025,24180,24336,24492,24649,24806,24964,25122,25281,25440,25600 };

static int16_t qmul(int16_t a, int16_t b)
{
    uint8_t neg = 0, ua, ub;
    int16_t r;
    if (a < 0) { a = -a; neg = 1; }
    if (b < 0) { b = -b; neg ^= 1; }
    ua = (uint8_t)a; ub = (uint8_t)b;
    r = (int16_t)(QSQ[(uint16_t)ua + ub] - QSQ[ua > ub ? ua - ub : ub - ua]);
    return neg ? -r : r;
}

/* Each product fits in 16 bits; only the difference needs 32. */
static int32_t cross32(int16_t ax, int16_t ay, int16_t bx, int16_t by)
{
    return (int32_t)qmul(ax, by) - qmul(ay, bx);
}

static int32_t dot32(int16_t ax, int16_t ay, int16_t bx, int16_t by)
{
    return (int32_t)qmul(ax, bx) + qmul(ay, by);
}

static int8_t sgn32(int32_t v) { return v > 0 ? 1 : (v < 0 ? -1 : 0); }

static uint8_t rnd(void)
{
    rng = rng * 25173u + 13849u;
    return (uint8_t)(rng >> 8);
}

#define ATX (hx + 8)        /* where the hose meets the diver (his belt) */
#define ATY (hy + 12)

/* ------------------------------------------------------------------ */
/* Text, HUD and banners (window layer)                                */
/* ------------------------------------------------------------------ */
static uint8_t font_tile(char c)
{
    if (c >= 'A' && c <= 'Z') return (uint8_t)(B_FONT + 1u + (uint8_t)(c - 'A'));
    if (c >= '0' && c <= '9') return (uint8_t)(B_FONT + 27u + (uint8_t)(c - '0'));
    switch (c) {
        case '!': return B_FONT + 37u;
        case ',': return B_FONT + 38u;
        case '.': return B_FONT + 39u;
        case '-': return B_FONT + 40u;
        case ':': return B_FONT + 41u;
    }
    return B_FONT;
}

static uint8_t rowbuf[20];
static void text_row(const char *s)            /* centred text on a dark row */
{
    uint8_t n = (uint8_t)strlen(s), x = (uint8_t)((20u - n) >> 1), i;
    memset(rowbuf, B_HUD_BLANK, 20);
    for (i = 0; i < n && x + i < 20u; i++) rowbuf[x + i] = font_tile(s[i]);
}

/* Banner: n text rows in the window, HUD row below them. */
static void win_row(uint8_t y)                 /* queued: no waiting on the LCD */
{
    uint8_t i, *w = (uint8_t *)(0x9C00u + ((uint16_t)y << 5));
    for (i = 0; i < 20u; i++) vram_put(w + i, rowbuf[i]);
}

static void banner(const char *a, const char *b, const char *c)
{
    uint8_t n = 0;
    if (a) { text_row(a); win_row(n++); }
    if (b) { text_row(b); win_row(n++); }
    if (c) { text_row(c); win_row(n++); }
    banner_rows = n;
    memset(hud_cache, 0xFF, 20);
    WY_REG = (uint8_t)(PLAY_H - n * 8u);
}

static void banner_off(void)
{
    banner_rows = 0;
    memset(hud_cache, 0xFF, 20);
    WY_REG = PLAY_H;
}

static uint16_t hose_used(void);

static void draw_hud(void)
{
    if (hud_cache[0] != 0xFFu && ((++hud_t & 3u) || (LY_REG >= 100u && LY_REG < 144u))) return;   /* every 4th frame, if there's time */
    uint8_t row[20], i, v, base;
    uint16_t used, t;
    uint8_t low = air < 25u * 60u && state == ST_PLAY && ((frame >> 3) & 1u);

    memset(row, B_HUD_BLANK, 20);
    t = air >> 3;                                      /* air (0..6000) -> 0..64 px: x * 11 / 1024 */
    v = (uint8_t)(((t << 3) + (t << 1) + t) >> 7);
    base = low ? B_AIR_LOW : B_AIR;
    for (i = 0; i < 8u; i++) {
        if (v >= 8u) { row[i] = base + 8u; v -= 8u; } else { row[i] = base + v; v = 0; }
    }
    used = hose_on ? hose_used() : 0u;
    v = used < hose_len ? (uint8_t)(hose_len - used) : 0u;
    v = (uint8_t)(((uint16_t)v * hose_k) >> 8);        /* 0..40 px */
    for (i = 9; i < 14u; i++) {
        if (v >= 8u) { row[i] = B_HOSEBAR + 8u; v -= 8u; } else { row[i] = B_HOSEBAR + v; v = 0; }
    }
    for (i = 0; i < HOSE_HP; i++) row[15 + i] = i < hp ? B_PIP_ON : B_PIP_OFF;
    row[18] = B_GEM_ICON;
    row[19] = font_tile((char)('0' + (ngems - ngot)));
    {                                                  /* queue only the tiles that changed */
        uint8_t *w = (uint8_t *)(0x9C00u + ((uint16_t)banner_rows << 5));
        for (i = 0; i < 20u; i++, w++)
            if (hud_cache[i] != row[i]) { hud_cache[i] = row[i]; vram_put(w, row[i]); }
    }
}

/* ------------------------------------------------------------------ */
/* Hose geometry                                                       */
/* ------------------------------------------------------------------ */
static uint16_t hose_used(void)
{
    return fixed_len + isqrt16(d2(ATX - chx[nch - 1], ATY - chy[nch - 1]));
}

static void attach_to(uint8_t i)
{
    pump = i;
    nch = 1;
    chx[0] = (uint8_t)(pumpx[i] + 8u);
    chy[0] = (uint8_t)(pumpy[i] + 8u);
    chlen[0] = 0;
    fixed_len = 0;
    fixed_dirty = 1;
    hose_dirty = 1;
    rcr_dirty = 1;
}

static void recompute_fixed(void)
{
    uint8_t k;
    fixed_len = 0;
    for (k = 1; k < nch; k++) fixed_len += chlen[k];
}

/* The hose bends at rocks it swings across and straightens when the diver comes back around. */
/* Per rock: position relative to the last pivot, and cross(diver - pivot, rock - pivot).
 * The diver moves exactly 1 px along one axis per step, so the cross product changes by
 * just +-(rock y) or -+(rock x): an addition, no multiply. A sign change means the hose
 * swung across the rock. Recomputed (with multiplies) only when the pivot changes. */

/* One 1 px step of the diver changes every rock's cross product by +-y or -+x of the rock
 * (mode 0: x+1, 1: x-1, 2: y+1, 3: y-1). Rocks whose sign flipped become candidates.
 * Kept small and separate so the compiler keeps it in registers. */
static uint8_t cand[MAX_ROCKS], ncand;
static int16_t cand_c0[MAX_ROCKS];
static uint8_t sw_mode, sw_i;
static int16_t sw_c0;
static void sweep_rocks(uint8_t mode) __naked   /* (assembly; RockTrack is 7 bytes: x y c act) */
{
    (void)mode;
    __asm
    ld (_sw_mode), a
    xor a, a
    ld (_ncand), a
    ld (_sw_i), a
    ld hl, #_rt
1$:
    ld a, (_sw_i)
    ld b, a
    ld a, (_nrocks)
    cp a, b
    ret z
    push hl
    ld de, #6
    add hl, de
    ld a, (hl)                  ; act
    pop hl
    or a, a
    jr z, 8$
    push hl
    ld a, (_sw_mode)
    cp a, #2
    jr nc, 2$
    inc hl                      ; mode 0/1: use y
    inc hl
2$:
    ld a, (hl+)
    ld c, a
    ld b, (hl)                  ; BC = step
    pop hl
    push hl
    ld de, #4
    add hl, de
    ld a, (hl+)
    ld e, a
    ld d, (hl)                  ; DE = c0
    dec hl
    ld a, e
    ld (_sw_c0), a
    ld a, d
    ld (_sw_c0+1), a
    ld a, (_sw_mode)
    cp a, #1
    jr z, 3$
    cp a, #2
    jr z, 3$
    ld a, e                     ; c1 = c0 + step
    add a, c
    ld c, a
    ld a, d
    adc a, b
    ld b, a
    jr 4$
3$:
    ld a, e                     ; c1 = c0 - step
    sub a, c
    ld c, a
    ld a, d
    sbc a, b
    ld b, a
4$:
    ld (hl), c
    inc hl
    ld (hl), b
    pop hl
    ld a, e                     ; candidate: c0 != 0 and (c1 == 0 or the sign flipped)
    or a, d
    jr z, 8$
    ld a, c
    or a, b
    jr z, 5$
    ld a, d
    xor a, b
    bit 7, a
    jr z, 8$
5$:
    push hl
    ld a, (_ncand)
    ld e, a
    ld d, #0
    ld hl, #_cand
    add hl, de
    ld a, (_sw_i)
    ld (hl), a
    ld hl, #_cand_c0
    add hl, de
    add hl, de
    ld a, (_sw_c0)
    ld (hl+), a
    ld a, (_sw_c0+1)
    ld (hl), a
    ld a, e
    inc a
    ld (_ncand), a
    pop hl
8$:
    ld de, #7
    add hl, de
    ld a, (_sw_i)
    inc a
    ld (_sw_i), a
    jp 1$
    __endasm;
}

static void rocks_reset(int16_t hx_, int16_t hy_)
{
    uint8_t k;
    int16_t px = chx[nch - 1], py = chy[nch - 1], reach = (int16_t)hose_len - (int16_t)fixed_len;
    uint16_t r2 = SQ[reach < 0 ? 0 : (reach > 255 ? 255 : (uint8_t)reach)];
    RockTrack *r = rt;
    for (k = 0; k < nrocks; k++, r++) {
        r->x = (int16_t)(rockx[k] + 8u) - px;
        r->y = (int16_t)(rocky[k] + 8u) - py;
        /* a rock further from the pivot than the hose can reach can't be swept; skipping it
         * also keeps |cross| <= reach * reach <= 160 * 160, so 16 bits are enough */
        r->act = d2(r->x, r->y) <= r2;
        r->c = r->act ? (int16_t)cross32(hx_ - px, hy_ - py, r->x, r->y) : 0;
    }
    rcr_dirty = 0;
}

/* The hose bends at rocks it swings across and straightens when the diver comes back around.
 * (h0 -> h1 is a single 1 px step along x or y.) */
static void update_chain(int16_t h0x, int16_t h0y, int16_t h1x, int16_t h1y)
{
    uint8_t k, best = 0xFF, changed = 0, len, best_len = 1;
    int16_t px, py, cx, cy, ex = h1x - h0x, ey = h1y - h0y, c0, c1, best_c0 = 0, best_c1 = 0;

    while (nch > 1) {                                  /* unwrap */
        k = (uint8_t)(nch - 1u);
        if (sgn32(cross32(chx[k] - chx[k - 1], chy[k] - chy[k - 1], h1x - chx[k], h1y - chy[k])) == -chside[k]) {
            nch--; changed = 1;
        } else break;
    }
    if (changed) { recompute_fixed(); fixed_dirty = 1; rcr_dirty = 1; return; }  /* no re-catch on the same step */
    if (rcr_dirty) rocks_reset(h0x, h0y);

    px = chx[nch - 1]; py = chy[nch - 1];
    sweep_rocks(ex > 0 ? 0u : (ex < 0 ? 1u : (ey > 0 ? 2u : 3u)));
    {
        uint8_t j;
    for (j = 0; j < ncand; j++) {                      /* wrap: rocks the hose swung across */
        k = cand[j];
        c0 = cand_c0[j];
        c1 = rt[k].c;
        cx = rt[k].x; cy = rt[k].y;
        if (dot32(cx, cy, h0x - px, h0y - py) <= 0 && dot32(cx, cy, h1x - px, h1y - py) <= 0) continue;   /* behind the pivot */
        /* rock beyond the diver? (side of the 1 px step line; no multiply needed) */
        {
            int16_t e  = ex * (cy + py - h0y) - ey * (cx + px - h0x);
            int16_t ep = ex * (py - h0y) - ey * (px - h0x);
            if (e && ((e > 0) != (ep > 0))) continue;
        }
        if (nch >= MAX_PIV) continue;
        len = isqrt16(d2(cx, cy));
        if (!len) len = 1;
        /* several at once: smallest angle from the old line (|c0| / len smallest) */
        if (best == 0xFF || (c0 < 0 ? -c0 : c0) * (int32_t)best_len < (best_c0 < 0 ? -best_c0 : best_c0) * (int32_t)len) {
            best = k; best_c0 = c0; best_c1 = c1; best_len = len;
        }
    }
    }
    if (best != 0xFF) {
        chx[nch] = (uint8_t)(rockx[best] + 8u);
        chy[nch] = (uint8_t)(rocky[best] + 8u);
        chside[nch] = best_c1 ? (best_c1 > 0 ? -1 : 1) : (best_c0 > 0 ? 1 : -1);
        chlen[nch] = best_len;
        nch++;
        recompute_fixed();
        fixed_dirty = 1;
        rcr_dirty = 1;
    }
}

/* Feet box (x+4..x+11, y+10..y+15) against rock and pump cells: base_tile > seabed = solid.
 * Rocks and pumps fill whole 8x8 cells, so checking the box corners' cells is enough. */
static const uint16_t ROWOFF[CELLS_H] = {
    0, 20, 40, 60, 80, 100, 120, 140, 160, 180, 200, 220, 240, 260, 280, 300, 320 };
static uint8_t blocked(int16_t x, int16_t y)
{
    uint8_t c0 = (uint8_t)(x + 4) >> 3, c1 = (uint8_t)(x + 11) >> 3;
    const uint8_t *r0 = &base_tile[ROWOFF[(uint8_t)(y + 10) >> 3]];
    const uint8_t *r1 = &base_tile[ROWOFF[(uint8_t)(y + 15) >> 3]];
    return r0[c0] > 4u || r0[c1] > 4u || r1[c0] > 4u || r1[c1] > 4u;
}

/* One pixel step on one axis: walls, screen edges and hose length. */
static void try_move(int16_t nx, int16_t ny)
{
    int16_t px, py, r;
    uint16_t dnew, dold, r2;
    if (nx < 0 || nx > SCR_W - 16 || ny < 0 || ny > PLAY_H - 16) return;
    if (blocked(nx, ny)) return;
    px = chx[nch - 1]; py = chy[nch - 1];
    dnew = d2(nx + 8 - px, ny + 12 - py);
    dold = d2(ATX - px, ATY - py);
    r = (int16_t)hose_len - (int16_t)fixed_len;
    if (r < 0) r = 0;
    r2 = SQ[r > 255 ? 255 : (uint8_t)r];
    if (dnew > r2 && dnew > dold) return;               /* full length: only back toward the line */
    {
        int16_t h0x = ATX, h0y = ATY;
        hx = nx; hy = ny;
        update_chain(h0x, h0y, ATX, ATY);
    }
    hose_dirty = 1;
}

/* Nearest point on the hose to (mx, my). Returns squared distance. */
static uint8_t near_seg;
static int16_t near_x, near_y;
static uint16_t nearest_on_hose(int16_t mx, int16_t my)
{
    uint8_t k;
    int16_t ax, ay, bx, by, vx, vy, wx, wy, qx, qy, lo, hi;
    int32_t dot, vv;
    uint16_t best = 0xFFFFu, d;
    for (k = 1; k <= nch; k++) {
        ax = chx[k - 1]; ay = chy[k - 1];
        if (k < nch) { bx = chx[k]; by = chy[k]; } else { bx = ATX; by = ATY; }
        lo = ax < bx ? ax : bx; hi = ax < bx ? bx : ax;
        if (mx < lo - 8 || mx > hi + 8) continue;      /* quick reject */
        lo = ay < by ? ay : by; hi = ay < by ? by : ay;
        if (my < lo - 8 || my > hi + 8) continue;
        vx = bx - ax; vy = by - ay; wx = mx - ax; wy = my - ay;
        dot = dot32(wx, wy, vx, vy);
        vv = dot32(vx, vy, vx, vy);
        if (!vv || dot <= 0) { qx = ax; qy = ay; }
        else if (dot >= vv) { qx = bx; qy = by; }
        else { qx = ax + (int16_t)(vx * dot / vv); qy = ay + (int16_t)(vy * dot / vv); }
        d = d2(mx - qx, my - qy);
        if (d < best) { best = d; near_seg = k; near_x = qx; near_y = qy; }
    }
    return best;
}

/* Point on the hose at distance s from the pump. */
static void point_at(uint16_t s, int16_t *ox, int16_t *oy)
{
    uint8_t k, l;
    int16_t ax, ay, bx, by;
    for (k = 1; k <= nch; k++) {
        ax = chx[k - 1]; ay = chy[k - 1];
        if (k < nch) { bx = chx[k]; by = chy[k]; l = chlen[k]; }
        else { bx = ATX; by = ATY; l = isqrt16(d2(bx - ax, by - ay)); }
        if (s <= l) {
            if (!l) { *ox = ax; *oy = ay; return; }
            *ox = ax + qmul(bx - ax, (int16_t)s) / (int16_t)l;
            *oy = ay + qmul(by - ay, (int16_t)s) / (int16_t)l;
            return;
        }
        s -= l;
    }
    *ox = ATX; *oy = ATY;
}

static void bite(void)
{
    uint8_t k;
    uint16_t s = 0;
    if (hp) { hp--; return; }
    if (nholes >= MAX_HOLES) return;
    for (k = 1; k < near_seg; k++) s += chlen[k];
    s += isqrt16(d2(near_x - chx[near_seg - 1], near_y - chy[near_seg - 1]));
    hole_t[nholes] = (uint8_t)(1u + (nholes * 7u) % 18u);
    holes[nholes++] = s;
    hose_dirty = 1;
}

/* ------------------------------------------------------------------ */
/* Pumps                                                               */
/* ------------------------------------------------------------------ */
static void pump_tiles(uint8_t i, uint8_t t)
{
    uint8_t cx = pumpx[i] >> 3, cy = (uint8_t)(pumpy[i] >> 3);
    set_bkg_tile_xy(cx, cy + 1u, t);
    set_bkg_tile_xy(cx + 1u, cy + 1u, (uint8_t)(t + 1u));
    base_tile[(uint16_t)(cy + 1u) * CELLS_W + cx] = t;
    base_tile[(uint16_t)(cy + 1u) * CELLS_W + cx + 1u] = (uint8_t)(t + 1u);
}

static void pump_lights(void)
{
    uint8_t want = !hose_on ? 0u : (((frame >> 4) & 1u) ? 2u : 1u), i;
    if (want == pump_light) return;
    pump_light = want;
    for (i = 0; i < npumps; i++)
        pump_tiles(i, (i == pump && want) ? (want == 1u ? B_PUMP_ON : B_PUMP_ON2) : B_PUMP_OFF);
}

static void start_reel(int8_t to, uint8_t free)
{
    uint8_t k;
    for (k = 0; k < nch; k++) { rx[k] = chx[k]; ry[k] = chy[k]; rlen[k] = chlen[k]; }
    nr = nch;
    reel_eaten = 0;
    reel_to = to;
    reel_free = free;
    reeling = 1;
    fixed_dirty = 1;
    hose_dirty = 1;
}

static uint16_t reel_total(void)
{
    uint8_t k;
    uint16_t t = 0;
    for (k = 1; k < nr; k++) t += rlen[k];
    return t + isqrt16(d2(ATX - rx[nr - 1], ATY - ry[nr - 1]));
}

/* ------------------------------------------------------------------ */
/* Bubbles                                                             */
/* ------------------------------------------------------------------ */
static void add_bubble(int16_t x, int16_t y, uint8_t vy, int8_t vx, uint8_t life, uint8_t wob, uint8_t kind)
{
    uint8_t i;
    for (i = 0; i < MAX_BUBBLES; i++)
        if (!blife[i]) {
            bx16[i] = x << 4; by16[i] = y << 4; bvy[i] = vy; bvx[i] = vx;
            bt[i] = 0; blife[i] = life; bwob[i] = wob; bkind[i] = kind;
            return;
        }
}

static void update_bubbles(void)
{
    uint8_t i;
    for (i = 0; i < MAX_BUBBLES; i++) {
        if (!blife[i]) continue;
        bt[i]++;
        by16[i] -= bvy[i];
        bx16[i] += bvx[i] + (((bt[i] / bwob[i]) & 1u) ? 2 : -2);
        if (bt[i] >= blife[i] || by16[i] < -64) blife[i] = 0;
    }
}

/* ------------------------------------------------------------------ */
/* Level setup                                                         */
/* ------------------------------------------------------------------ */
static void load_level(uint8_t n)
{
    const LevelDef *d = &levels[n];
    uint8_t r, c, k, x, y;
    char ch;
    uint16_t i;

    DISPLAY_OFF;
    lv = n;
    npumps = nrocks = ngems = 0;
    for (r = 0; r < 8; r++)
        for (c = 0; c < 10; c++) {
            ch = d->map[r][c];
            x = (uint8_t)(c * 16u); y = (uint8_t)(r * 16u);
            if (ch == 'S') {                              /* start pump goes first */
                for (k = npumps; k > 0; k--) { pumpx[k] = pumpx[k - 1]; pumpy[k] = pumpy[k - 1]; }
                pumpx[0] = x; pumpy[0] = y; npumps++;
            } else if (ch == 'P') { pumpx[npumps] = x; pumpy[npumps] = y; npumps++; }
            else if (ch == 'R') { rockx[nrocks] = x; rocky[nrocks] = y; nrocks++; }
            else if (ch == 'G') { gemx[ngems] = (uint8_t)(x + 4u); gemy[ngems] = (uint8_t)(y + 4u); gemgot[ngems] = 0; ngems++; }
        }
    hose_len = d->hose_len;
    hose_k = (uint8_t)((40u * 256u) / hose_len);
    for (k = 0; k < ngems; k++) gem_phase[k] = (uint16_t)(k * (GLIMMER_EVERY / ngems));
    glim_t = 0;

    /* seabed: mostly plain, some ripples (fixed pattern per level) */
    rng = (uint16_t)(0x1234u + n * 977u);
    for (i = 0; i < NCELLS; i++) {
        k = rnd();
        base_tile[i] = k < 180u ? B_GROUND : (uint8_t)(B_GROUND + 1u + (k & 3u));
    }
    for (k = 0; k < nrocks; k++) {
        i = (uint16_t)(rocky[k] >> 3) * CELLS_W + (rockx[k] >> 3);
        base_tile[i] = B_ROCK; base_tile[i + 1] = B_ROCK + 1u;
        base_tile[i + CELLS_W] = B_ROCK + 2u; base_tile[i + CELLS_W + 1] = B_ROCK + 3u;
    }
    for (k = 0; k < npumps; k++) {
        i = (uint16_t)(pumpy[k] >> 3) * CELLS_W + (pumpx[k] >> 3);
        base_tile[i] = B_PUMP_TOP; base_tile[i + 1] = B_PUMP_TOP + 1u;
        base_tile[i + CELLS_W] = B_PUMP_OFF; base_tile[i + CELLS_W + 1] = B_PUMP_OFF + 1u;
    }
    set_bkg_tiles(0, 0, CELLS_W, CELLS_H, base_tile);
    pool_init();

    nfish = d->nfish;
    for (k = 0; k < nfish; k++) {
        fv[k] = d->fish[k][1];
        fx16[k] = fv[k] > 0 ? 0 : (int16_t)(144 * 16);
        fy[k] = (uint8_t)(d->fish[k][0] * 16 + 4);
        fst[k] = F_SWIM; ft[k] = 0; fcd[k] = 0;
    }
    for (k = 0; k < MAX_BUBBLES; k++) { blife[k] = 0; bub_shown[k] = 1; }
    for (k = 0; k < MAX_GEMS; k++) gem_tile[k] = 0xFF;

    start_x = (uint8_t)(pumpx[0] + 18u); start_y = pumpy[0];
    hx = start_x; hy = -24;
    dir = DIR_UP; step = 0; anim_t = 0; subx = suby = 0;
    air = AIR_MAX; hp = HOSE_HP; nholes = 0; breath_t = BREATH_EVERY; hud_t = 0; hose_phase = 0;
    ngot = 0; cleared = 0; msg_t = 0;
    reeling = 0; hose_on = 0; pump_light = 0xFF;
    attach_to(0);
    hose_dirty = 1;
    state = ST_INTRO; sub = SUB_LOWER; pt = 0; hook_y = hy + 4;
    frame = 0;

    {
        static char name[8] = "LEVEL 1";
        name[6] = (char)('1' + n);
        banner(name, 0, 0);
    }
    SHOW_BKG; SHOW_WIN; SHOW_SPRITES;
    DISPLAY_ON;
}

/* ------------------------------------------------------------------ */
/* Per-frame logic                                                     */
/* ------------------------------------------------------------------ */
static void update_fish(void)
{
    uint8_t k, can_bite = state == ST_PLAY && hose_on && !reeling, hy0 = 255, hy1 = 0, a;
    if (can_bite) {                                    /* vertical extent of the hose */
        for (k = 0; k < nch; k++) { a = chy[k]; if (a < hy0) hy0 = a; if (a > hy1) hy1 = a; }
        a = (uint8_t)ATY; if (a < hy0) hy0 = a; if (a > hy1) hy1 = a;
    }
    int16_t mx, my, x;
    uint16_t d;
    for (k = 0; k < nfish; k++) {
        x = fx16[k] >> 4;
        mx = fv[k] < 0 ? x + 2 : x + 13;
        my = fy[k] + 3;
        switch (fst[k]) {
        case F_SWIM:
            fx16[k] += fv[k];
            if (fx16[k] <= 0) { fx16[k] = 0; if (fv[k] < 0) fv[k] = -fv[k]; }
            if (fx16[k] >= 144 * 16) { fx16[k] = 144 * 16; if (fv[k] > 0) fv[k] = -fv[k]; }
            if (fcd[k]) { fcd[k]--; break; }
            if (!can_bite || ((frame + k) & 1u)) break;   /* check every other frame */
            if ((uint8_t)my + 6u < hy0 || (uint8_t)my > hy1 + 6u) break;   /* hose nowhere near this row */
            d = nearest_on_hose(mx, my);
            if (d <= SPOT_DIST2 && (near_x - mx) * (fv[k] < 0 ? -1 : 1) >= -1) { fst[k] = F_STOP; ft[k] = 0; }
            break;
        case F_STOP:
            if (!can_bite) { fst[k] = F_SWIM; break; }
            if (++ft[k] >= WINDUP) {
                d = nearest_on_hose(mx, my);
                if (d > SPOT_DIST2 + 20u) { fst[k] = F_SWIM; break; }   /* hose moved away */
                fjx[k] = (int8_t)(near_x - mx); fjy[k] = (int8_t)(near_y - my);
                fst[k] = F_JUMP; ft[k] = 0;
            }
            break;
        case F_JUMP:                                       /* 2 frames: half, then the rest */
            if (ft[k] == 0) { fx16[k] += (int16_t)(fjx[k] / 2) * 16; fy[k] += fjy[k] / 2; ft[k] = 1; }
            else {
                fx16[k] += (int16_t)(fjx[k] - fjx[k] / 2) * 16; fy[k] += fjy[k] - fjy[k] / 2;
                if (can_bite) {
                    x = fx16[k] >> 4;
                    if (nearest_on_hose(fv[k] < 0 ? x + 2 : x + 13, fy[k] + 3) != 0xFFFFu) bite();
                }
                fst[k] = F_HOLD; ft[k] = 0; fflap[k] = 0; fflap_t[k] = 0;
            }
            break;
        case F_HOLD:
            ft[k]++;
            if (++fflap_t[k] >= FLAP_FRAMES) { fflap_t[k] = 0; fflap[k] ^= 1u; }
            if (ft[k] >= HOLD_FRAMES || !can_bite ||
                ((ft[k] & 3u) == 0 && nearest_on_hose(mx, my) > GRIP_DIST2)) {
                fst[k] = F_SWIM; fv[k] = -fv[k]; fcd[k] = BITE_COOLDOWN;
            }
            break;
        }
    }
}

static void update_play_move(void)
{
    int8_t dx = 0, dy = 0;
    uint16_t sp;
    uint8_t steps;
    if (state == ST_PLAY && !reeling) {
        if (keys & J_LEFT) dx--;
        if (keys & J_RIGHT) dx++;
        if (keys & J_UP) dy--;
        if (keys & J_DOWN) dy++;
    }
    if (dx || dy) {
        dir = DIR_FROM[dy + 1][dx + 1];
        if (++anim_t >= 12u) { anim_t = 0; step = (step + 1u) & 3u; }
    } else if (state == ST_PLAY) { step = 0; anim_t = 11; }
    sp = (dx && dy) ? SPEED_DIAG : SPEED_STRAIGHT;
    suby = dy ? suby + sp : 0;
    subx = dx ? subx + sp : 0;
    steps = (uint8_t)(suby >> 8); suby &= 0xFFu;
    while (steps--) try_move(hx, hy + dy);
    steps = (uint8_t)(subx >> 8); subx &= 0xFFu;
    while (steps--) try_move(hx + dx, hy);
}

static void update_game(void)
{
    uint8_t k;
    int16_t qx, qy;

    frame++; pt++;
    if (++glim_t >= GLIMMER_EVERY) glim_t = 0;
    update_play_move();

    if (state == ST_PLAY) {
        /* 8-bit tests, all values offset by +16 so nothing goes below zero */
        uint8_t ux0 = (uint8_t)hx + 20u, ux1 = (uint8_t)hx + 27u, uy0 = (uint8_t)hy + 22u, uy1 = (uint8_t)hy + 31u, gx, gy;
        for (k = 0; k < ngems; k++) {
            if (gemgot[k]) continue;
            gx = gemx[k] + 16u; gy = gemy[k] + 16u;
            if (ux1 >= gx && ux0 <= gx + 7u && uy1 >= gy && uy0 <= gy + 7u) { gemgot[k] = 1; ngot++; }
        }
        if (!cleared && ngot == ngems) {
            cleared = 1; msg_t = MSG_FRAMES;
            banner("WELL DONE!", "NOW RETURN TO", "THE START PUMP");
        }
        if (!reeling)
            for (k = 0; k < npumps; k++) {
                gx = pumpx[k] + 16u; gy = pumpy[k] + 16u;
                if (!(ux1 >= gx - 3u && ux0 <= gx + 18u && uy1 >= gy - 3u && (uint8_t)(uy0 + 4u) <= gy + 18u)) continue;
                if (k == 0 && cleared) {                    /* home with every gem */
                    state = ST_OUTRO; sub = SUB_REEL; pt = 0;
                    if (msg_t) { msg_t = 0; banner_off(); }
                    start_reel(-1, 1);
                    break;
                }
                if (k != pump) { start_reel((int8_t)k, 0); break; }
            }
    }
    if (msg_t && !--msg_t) banner_off();

    if (reeling) {
        reel_eaten += REEL_SPEED;
        if (!reel_free && air > REEL_DRAIN) air -= REEL_DRAIN; else if (!reel_free) air = 0;
        hose_dirty = 1;
        if (reel_eaten >= reel_total()) {
            reeling = 0;
            if (reel_to >= 0) attach_to((uint8_t)reel_to);
            else { hose_on = 0; fixed_dirty = 1; }
            pump_light = 0xFF;
        }
    }
    if (state == ST_PLAY) {
        k = (uint8_t)(nholes * HOLE_DRAIN);
        air = air > k ? air - k : 0;
        if (!air) {                                       /* drowned */
            state = ST_DEAD; pt = 0; reeling = 0; banner_off();
        }
    }

    switch (state) {
    case ST_INTRO:                                        /* lowered in on the hook */
        if (sub == SUB_LOWER) {
            hy++;
            hx = start_x + SWAY[(pt >> 3) & 7u];
            hook_y = hy + 4;
            if (hy >= start_y) { hy = start_y; hx = start_x; sub = SUB_HOOKUP; pt = 0; }
        } else if (pt > 20u) {
            hook_y -= HOOK_SPEED;
            if (hook_y < -10) {
                hose_on = 1; attach_to(0); pump_light = 0xFF;
                state = ST_PLAY; pt = 0; dir = DIR_DOWN; banner_off();
            }
        }
        break;
    case ST_OUTRO:                                        /* reel in, hook down, lifted out */
        if (sub == SUB_REEL) { if (!reeling) { sub = SUB_HOOKDOWN; pt = 0; hook_y = -10; dir = DIR_UP; step = 0; } }
        else if (sub == SUB_HOOKDOWN) { hook_y += HOOK_SPEED; if (hook_y >= hy + 4) { hook_y = hy + 4; sub = SUB_PAUSE; pt = 0; } }
        else if (sub == SUB_PAUSE) { if (pt > 20u) { sub = SUB_LIFT; pt = 0; lift_x = hx; } }
        else {
            hy--; hook_y = hy + 4;
            hx = lift_x + SWAY[(pt >> 3) & 7u];
            if (hy < -26) {
                pt = 0;
                if (lv + 1u < NUM_LEVELS) { state = ST_DONE; banner("LEVEL CLEAR!", 0, 0); }
                else { state = ST_END; banner("ALL LEVELS CLEAR!", "WELL DONE, DIVER", "PRESS START"); }
            }
        }
        break;
    case ST_DONE:
        if (pt >= 150u) { load_level((uint8_t)(lv + 1u)); return; }
        break;
    case ST_DEAD:                                         /* burst of bubbles, then stillness */
        if (pt <= 32u && !(pt & 3u))
            add_bubble(hx + 4 + (rnd() & 7u), hy + 6, (uint8_t)(10u + (rnd() & 7u)), (int8_t)((rnd() & 7u) - 3), 200, 6, 2);
        if (pt == 120u) banner("OUT OF AIR", "PRESS A TO RETRY", 0);
        if (pt > 120u && (keys & (J_A | J_START)) && !(prev_keys & (J_A | J_START))) { load_level(lv); return; }
        break;
    case ST_END:
        if ((keys & J_START) && !(prev_keys & J_START)) { state = ST_TITLE; pt = 0xFFFFu; return; }
        break;
    }

    update_fish();

    if (state == ST_PLAY)                                 /* leaking holes bubble from their spot */
        for (k = 0; k < nholes; k++)
            if (!--hole_t[k]) {
                hole_t[k] = 18;
                if (reeling) { qx = ATX; qy = ATY; } else point_at(holes[k], &qx, &qy);
                add_bubble(qx, qy, 8, 0, 70, 8, 0);
            }
    if (!--breath_t) breath_t = BREATH_EVERY;
    if (state != ST_DEAD && hy > 0 && breath_t == BREATH_EVERY)
        add_bubble(ATX, hy + 1, 4, 0, 120, 16, 0);       /* calm breathing bubble */
    update_bubbles();
}

/* ------------------------------------------------------------------ */
/* Drawing                                                             */
/* ------------------------------------------------------------------ */
/* The hose is redrawn in steps spread over frames so the game keeps 60 fps:
 *   phase 0: clear the layers and make a list of short segments (<= 24 px) to draw
 *   phase 1: draw as many segments as fit in what's left of each frame, then the holes
 *   phase 2: upload changed tiles (also spread over frames if needed)
 * The positions are captured in phase 0, so the drawing is consistent even if the
 * diver moves meanwhile; the hose just catches up a frame or two later. */
#define MAX_SEG 32
static int16_t sgx0[MAX_SEG], sgy0[MAX_SEG], sgx1[MAX_SEG], sgy1[MAX_SEG];
static uint8_t sgfix[MAX_SEG], sglen[MAX_SEG], nseg, seg_i, seg_holes;

static void add_seg(int16_t ax, int16_t ay, int16_t bx, int16_t by, uint8_t fixed)
{
    int16_t dx = bx - ax, dy = by - ay, px = ax, py = ay, qx, qy, accx = 0, accy = 0;
    uint8_t adx = (uint8_t)(dx < 0 ? -dx : dx), ady = (uint8_t)(dy < 0 ? -dy : dy);
    uint8_t len = adx > ady ? adx : ady, sh, parts, i;
    /* split into 1, 2, 4, 8 or 16 equal parts of at most 24 px (shifts only, no divide);
     * the whole hose is at most 160 px, so this stays within MAX_SEG */
    sh = len <= 24u ? 0u : (len <= 48u ? 1u : (len <= 96u ? 2u : (len <= 192u ? 3u : 4u)));
    parts = (uint8_t)(1u << sh);
    for (i = 1; i <= parts && nseg < MAX_SEG; i++) {
        accx += dx; accy += dy;                           /* = d * i */
        if (i == parts) { qx = bx; qy = by; }
        else { qx = ax + (accx >> sh); qy = ay + (accy >> sh); }
        sgx0[nseg] = px; sgy0[nseg] = py; sgx1[nseg] = qx; sgy1[nseg] = qy;
        sgfix[nseg] = fixed; sglen[nseg] = (uint8_t)((len >> sh) + 1u);
        nseg++;
        px = qx; py = qy;
    }
}

static void draw_holes(void)
{
    uint8_t k;
    int16_t qx, qy;
    for (k = 0; k < nholes; k++) { point_at(holes[k], &qx, &qy); hose_blob(qx, qy); }
}

/* Current scanline as frame time: the vblank lines (144-153) count as the start of the frame. */
#define HOSE_START_LINES 32u
static uint8_t fline(void) { uint8_t l = LY_REG; return l >= 144u ? 0u : l; }

static void draw_hose_step(void)
{
    uint8_t k, l, rebuild, ly;
    uint16_t skip;
    int16_t ax, ay, bx, by;

    if (hose_phase == 2) { if (hose_commit()) hose_phase = 0; return; }
    if (hose_phase == 1) {
        while (seg_i < nseg) {
            ly = fline();
            l = sglen[seg_i];
            if (ly + l + 4u >= hose_ly_end) return;   /* <= ~1 scanline per pixel, even when new tiles are needed */
            hose_line(sgx0[seg_i], sgy0[seg_i], sgx1[seg_i], sgy1[seg_i], sgfix[seg_i]);
            seg_i++;
        }
        if (seg_holes) {
            if (fline() + 8u >= hose_ly_end) return;
            draw_holes();
            seg_holes = 0;
        }
        hose_phase = 2;
        if (hose_commit()) hose_phase = 0;             /* start uploading right away */
        return;
    }
    if (!(fixed_dirty || hose_dirty) || vram_pending() >= 120u
        || fline() + hose_begin_cost(fixed_dirty) + 16u >= hose_ly_end) return;

    rebuild = fixed_dirty;
    hose_begin(rebuild);
    nseg = 0; seg_i = 0; seg_holes = 0;
    if (rebuild) {
        if (hose_on && !reeling)
            for (k = 1; k < nch; k++) add_seg(chx[k - 1], chy[k - 1], chx[k], chy[k], 1);
        fixed_dirty = 0;
    }
    if (hose_on) {
        if (reeling) {                                    /* shrinking from the pump end */
            skip = reel_eaten;
            for (k = 1; k <= nr; k++) {
                ax = rx[k - 1]; ay = ry[k - 1];
                if (k < nr) { bx = rx[k]; by = ry[k]; l = rlen[k]; }
                else { bx = ATX; by = ATY; l = isqrt16(d2(bx - ax, by - ay)); }
                if (skip >= l) { skip -= l; continue; }
                if (l) { ax += (int16_t)(bx - ax) * (int16_t)skip / (int16_t)l; ay += (int16_t)(by - ay) * (int16_t)skip / (int16_t)l; }
                skip = 0;
                add_seg(ax, ay, bx, by, 0);
            }
        } else {
            add_seg(chx[nch - 1], chy[nch - 1], ATX, ATY, 0);
            seg_holes = nholes ? 1 : 0;
        }
    }
    hose_dirty = 0;
    hose_phase = 1;
}

/* Sprites are written straight into GBDK's shadow OAM (copied to the hardware every vblank). */
#define OAM(n) shadow_OAM[n]
static void hide(uint8_t n) { OAM(n).y = 0; }
#define spr(n, X, Y, T) do { volatile struct OAM_item_t *o_ = &shadow_OAM[n]; \
    o_->y = (uint8_t)((Y) + 16); o_->x = (uint8_t)((X) + 8); o_->tile = (T); } while (0)
static uint8_t hook_shown, arrow_shown;
static const uint8_t GLIM_TILE[GLIMMER_LEN] = {
    T_GEM + 2, T_GEM + 2, T_GEM + 2, T_GEM + 2, T_GEM + 2, T_GEM + 2,
    T_GEM + 4, T_GEM + 4, T_GEM + 4, T_GEM + 4, T_GEM + 4, T_GEM + 4,
    T_GEM + 6, T_GEM + 6, T_GEM + 6, T_GEM + 6, T_GEM + 6, T_GEM + 6,
    T_GEM + 4, T_GEM + 4, T_GEM + 4, T_GEM + 4, T_GEM + 4, T_GEM + 4 };

static void draw_sprites(void)
{
    uint8_t k, t, frame_t, prop;
    int16_t x, y, ly;

    /* diver */
    if (state == ST_DEAD) { t = T_LYING; y = hy + 2; }
    else { t = (uint8_t)(hero_anim[dir][step] << 2); y = hy; }
    if (y > -16 && state != ST_DONE && state != ST_END) {
        spr(OAM_DIVER, hx, y, t);
        spr(OAM_DIVER + 1u, hx + 8, y, t + 2u);
    } else { hide(OAM_DIVER); hide(OAM_DIVER + 1u); }

    /* fish: two 8x16 sprites; facing right = both flipped and swapped */
    for (k = 0; k < MAX_FISH; k++) {
        if (k >= nfish || state >= ST_DONE && state != ST_DEAD) { hide(OAM_FISH + k * 2u); hide(OAM_FISH + k * 2u + 1u); continue; }
        frame_t = (fst[k] == F_HOLD && fflap[k]) ? T_FISH + 4u : T_FISH;
        x = fx16[k] >> 4;
        if (fv[k] > 0) {
            prop = S_PALETTE | S_FLIPX;
            spr(OAM_FISH + k * 2u, x, fy[k], frame_t + 2u);
            spr(OAM_FISH + k * 2u + 1u, x + 8, fy[k], frame_t);
        } else {
            prop = S_PALETTE;
            spr(OAM_FISH + k * 2u, x, fy[k], frame_t);
            spr(OAM_FISH + k * 2u + 1u, x + 8, fy[k], frame_t + 2u);
        }
        OAM(OAM_FISH + k * 2u).prop = prop;
        OAM(OAM_FISH + k * 2u + 1u).prop = prop;
    }

    /* gems, glimmering in turn */
    for (k = 0; k < MAX_GEMS; k++) {
        t = 0;                                         /* 0 = hidden */
        if (k < ngems && !gemgot[k] && state != ST_DONE && state != ST_END) {
            t = T_GEM;
            if (glim_t >= gem_phase[k] && glim_t - gem_phase[k] < GLIMMER_LEN)
                t = GLIM_TILE[(uint8_t)(glim_t - gem_phase[k])];   /* star size 1, 2, 3, 2 */
        }
        if (t == gem_tile[k]) continue;                /* unchanged */
        gem_tile[k] = t;
        if (t) spr(OAM_GEM + k, gemx[k], gemy[k], t); else hide(OAM_GEM + k);
    }

    /* arrow over the start pump once every gem is collected */
    if (cleared && state == ST_PLAY) {
        y = pumpy[0] >= 8u ? pumpy[0] - 6 + ((frame >> 4) & 1u) : pumpy[0] + 18 - ((frame >> 4) & 1u);
        spr(OAM_ARROW, pumpx[0] + 5, y, T_ARROW);
        arrow_shown = 1;
    } else if (arrow_shown) { hide(OAM_ARROW); arrow_shown = 0; }

    /* hook and line during the intro / outro */
    if ((state == ST_INTRO) || (state == ST_OUTRO && sub != SUB_REEL)) {
        x = hx + 8 - 3;                                    /* line is drawn at tile column 3 */
        y = hook_y - 6;                                    /* hook tip is tile row 6 */
        spr(OAM_HOOK, x, y, T_HOOK);
        ly = y;
        for (k = 0; k < 9u; k++) {
            ly -= 16;
            if (ly > -16) spr(OAM_LINE + k, x, ly, T_LINE); else hide(OAM_LINE + k);
        }
        hook_shown = 1;
    } else if (hook_shown) {
        hide(OAM_HOOK);
        for (k = 0; k < 9u; k++) hide(OAM_LINE + k);
        hook_shown = 0;
    }

    /* bubbles */
    for (k = 0; k < MAX_BUBBLES; k++) {
        if (!blife[k]) { if (bub_shown[k]) { hide(OAM_BUBBLE + k); bub_shown[k] = 0; } continue; }
        bub_shown[k] = 1;
        y = (by16[k] >> 4) - 3;
        if (y < -8) { hide(OAM_BUBBLE + k); continue; }
        t = bkind[k] == 2 ? T_BUB_BIG : (bt[k] > 25u ? T_BUB_RING : T_BUB_SMALL);
        spr(OAM_BUBBLE + k, (bx16[k] >> 4) - 3, y, t);
    }
}

/* ------------------------------------------------------------------ */
/* Title screen                                                        */
/* ------------------------------------------------------------------ */
static void title_text(uint8_t y, const char *s)
{
    text_row(s);
    set_bkg_tiles(0, y, 20, 1, rowbuf);
}

static void show_title(void)
{
    uint8_t k;
    static char lvl[] = "LEVEL 1";
    DISPLAY_OFF;
    pool_init();                                      /* drop any queued hose tile writes */
    for (k = 0; k < 18u; k++) title_text(k, "");
    title_text(3, "T E T H E R");
    title_text(5, "A DEEP SEA DIVE");
    title_text(9, "PRESS START");
    lvl[6] = (char)('1' + sel_level);
    title_text(11, lvl);
    title_text(13, "LEFT RIGHT: LEVEL");
    title_text(15, "HERO ART: GIBBONGL");
    title_text(16, "CC0");
    for (k = 0; k < NUM_OAM; k++) hide(k);
    HIDE_WIN;
    SHOW_BKG; SHOW_SPRITES;
    DISPLAY_ON;
}

static void update_title(void)
{
    uint8_t pressed = keys & (uint8_t)~prev_keys;
    static char lvl[] = "LEVEL 1";
    if (pressed & (J_LEFT | J_RIGHT)) {
        if (pressed & J_RIGHT) sel_level = (uint8_t)((sel_level + 1u) % NUM_LEVELS);
        else sel_level = (uint8_t)((sel_level + NUM_LEVELS - 1u) % NUM_LEVELS);
        lvl[6] = (char)('1' + sel_level);
        title_text(11, lvl);
    }
    if (pressed & (J_START | J_A)) load_level(sel_level);
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */
void main(void)
{
    DISPLAY_OFF;
    LCDC_REG = (uint8_t)((LCDC_REG & (uint8_t)~LCDCF_BG8000) | LCDCF_WIN9C00 | LCDCF_BG9800 | LCDCF_OBJ16);
    set_sprite_data(0, 128, spr_tiles);
    memcpy((uint8_t *)0x9000u, bg_tiles, B_NUM_TILES * 16u);   /* BG indices 0..89 (signed mode) */
    BGP_REG = 0xE4u;
    OBP0_REG = 0xD0u;      /* diver: 1 = white (face), 2 = light, 3 = black */
    OBP1_REG = 0xE4u;      /* fish, gems, bubbles, hook: 1 light, 2 dark, 3 black */
    WX_REG = 7; WY_REG = PLAY_H;
    SPRITES_8x16;
    {
        uint8_t k;
        for (k = OAM_GEM; k < NUM_OAM; k++) shadow_OAM[k].prop = S_PALETTE;   /* OBP1 */
    }
    state = ST_TITLE;
    show_title();

    while (1) {
        wait_vbl_done();
        vram_flush();
        prev_keys = keys;
        keys = joypad();
        if (state == ST_TITLE) {
            if (pt == 0xFFFFu) { pt = 0; show_title(); }
            update_title();
            continue;
        }
        /* the hose gets the start of the frame (so it keeps up even on busy levels),
         * the game logic the middle, and the hose whatever is left at the end */
        hose_ly_end = HOSE_START_LINES;
        draw_hose_step();
        update_game();
        if (state == ST_TITLE) continue;
        draw_sprites();
        hose_ly_end = 128;
        if (LY_REG < 120u) draw_hose_step();          /* not if the frame already ran long */
        pump_lights();
        draw_hud();
    }
}
