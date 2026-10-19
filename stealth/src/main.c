/*
 * main.c - stealth prototype for the Game Boy (DMG), GBDK-2020.
 *
 * A real-time tactics test: sneak past or take down the NPCs without being seen.
 *
 *   - The player moves in 8 directions with the D-pad; A takes down an NPC up close.
 *   - NPCs have a view cone (20 px, 30 degrees) that turns smoothly with them; walls block it.
 *   - NPC states:
 *       NORMAL      doing their duty (patrol route or guard post)
 *       SUSPICIOUS  glimpsed the player: leave the duty and search at random nearby
 *       ATTACK      saw the player for 3 s: go to the last known position, kill on arrival
 *
 * Credits: hero sprites "8-Directional Game Boy Character Template" by GibbonGL (CC0),
 * https://gibbongl.itch.io/8-directional-gameboy-character-template
 */
#include <gb/gb.h>
#include <stdint.h>
#include <string.h>
#include "game.h"
#include "gfx.h"

/* ---- tuning ---- */
#define PLAYER_SPEED   192u     /* 8.8 fixed point px per frame (0.75) */
#define PLAYER_DIAG    136u     /* x 1/sqrt(2) */
#define PATROL_SPEED   128u     /* 0.5 px/frame */
#define SEARCH_SPEED   160u
#define ATTACK_SPEED   256u
#define TURN_RATE      3u       /* facing units (256 per turn) per frame */
#define SPOT_FRAMES    180u     /* 3 s in the cone -> attack */
#define SEARCH_FRAMES  480u     /* 8 s of searching, then back to duty */
#define SEARCH_RANGE   3u       /* cells around the spot where the player was glimpsed */
#define KILL_DIST      9u       /* px between feet */
#define TAKEDOWN_DIST  12u
#define LOOK_RADIUS    20u      /* = cone length: "look again" on arrival */
#define GUARD_SWEEP    40u      /* +- facing units */

/* ---- map: 20 x 18 cells of 8 px.  # wall   . outside   , floor inside ---- */
static const char *const MAP[CELLS_H] = {
    "....................",
    "....................",
    "....................",
    "....############....",
    "....#,,,,#,,,,,#....",
    "....#,,,,,,,,,,,....",
    "....#,,,,,,,,,,,....",
    "....#,,,,#,,,,,#....",
    "....#,,,,###,,##....",
    "....#,,,,#,,,,,#....",
    "....,,,,,#,,,,,#....",
    "....,,,,,,,,,,,#....",
    "....#,,,,,,,,,,#....",
    "....#,,,,#,,,,,#....",
    "....#######,,###....",
    "....................",
    "....................",
    "....................",
};

uint8_t solid[PCELLS];
uint8_t base_tile[PCELLS];
const uint16_t ROWP[CELLS_H] = {
    PIDX(0, 0), PIDX(0, 1), PIDX(0, 2), PIDX(0, 3), PIDX(0, 4), PIDX(0, 5),
    PIDX(0, 6), PIDX(0, 7), PIDX(0, 8), PIDX(0, 9), PIDX(0, 10), PIDX(0, 11),
    PIDX(0, 12), PIDX(0, 13), PIDX(0, 14), PIDX(0, 15), PIDX(0, 16), PIDX(0, 17) };

/* ---- game state (global so the map file / emulator can watch them) ---- */
enum { G_TITLE, G_PLAY, G_DEAD, G_CLEAR };
enum { NS_NORMAL, NS_SUSPICIOUS, NS_ATTACK, NS_DOWN };
enum { DUTY_PATROL, DUTY_GUARD };
#define NNPC 2u

uint8_t gstate;
uint16_t frame;
uint8_t keys, prev_keys;
uint16_t pfx, pfy;              /* player feet centre, 8.8 */
uint8_t pdir, pstep, panim;

typedef struct {
    uint16_t fx, fy;            /* feet centre, 8.8 */
    uint8_t ang, want;          /* facing now / wanted, 0..255, 0 = east, 64 = south */
    uint8_t state, duty;
    uint8_t spot;               /* frames the player has been seen (detection) */
    uint8_t seen;               /* player in the cone this frame */
    uint16_t state_t;
    uint8_t tx, ty;             /* destination (px) */
    uint8_t nx, ny;             /* next point on the way (px) */
    uint8_t path_ok;            /* distance field ready for the destination */
    uint8_t arrived;
    uint8_t pause_t;
    uint8_t wp;                 /* patrol waypoint */
    uint8_t sx, sy;             /* search centre (px) */
    uint8_t lx, ly;             /* last known player position (px) */
    uint8_t retarget_t;
    uint8_t anim_t, step;
    uint8_t look_t;
} Npc;
Npc npc[NNPC];

/* patrol: counter-clockwise round the house, starting top right (feet positions, cell centres) */
static const uint8_t PATROL_X[4] = { 140, 20, 20, 140 };
static const uint8_t PATROL_Y[4] = { 12, 12, 132, 132 };
#define GUARD_X   100u
#define GUARD_Y   52u
#define GUARD_ANG 128u          /* facing west, towards the door */

/* ---- small maths ---- */
static uint16_t rng = 0xACE1u;
static uint8_t rnd(void) { rng = rng * 25173u + 13849u; return (uint8_t)(rng >> 8); }

static const uint8_t ATAN_T[33] = {     /* atan(i/32) in 1/256 turns */
    0, 1, 3, 4, 5, 6, 8, 9, 10, 11, 12, 13, 15, 16, 17, 18,
    19, 20, 21, 22, 23, 24, 25, 25, 26, 27, 28, 29, 29, 30, 31, 31, 32 };
static uint8_t atan2b(int16_t dx, int16_t dy)
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
static const uint8_t ROW_FROM_OCT[8] = { 6, 7, 0, 1, 2, 3, 4, 5 };   /* facing octant -> sheet row */
static const uint8_t DIR_FROM[3][3] = { { 3, 4, 5 }, { 2, 0, 6 }, { 1, 0, 7 } };

static uint8_t ad(uint8_t a, uint8_t b) { return a > b ? a - b : b - a; }

#define PX(f) ((uint8_t)((f) >> 8))
static uint8_t cell_solid(uint8_t x, uint8_t y) { return solid[ROWP[y >> 3] + (x >> 3)]; }

/* Line of sight between two points (walls block it). */
static uint8_t los(uint8_t x0, uint8_t y0, uint8_t x1, uint8_t y1)
{
    int16_t dx = (int16_t)x1 - x0, dy = (int16_t)y1 - y0, sx, sy, x, y;
    uint8_t n = ad(x0, x1) > ad(y0, y1) ? ad(x0, x1) : ad(y0, y1), i;
    if (!n) return 1;
    sx = (dx << 8) / n; sy = (dy << 8) / n;
    x = (int16_t)x0 << 8; y = (int16_t)y0 << 8;
    for (i = 0; i < n; i++) {
        x += sx; y += sy;
        if (cell_solid((uint8_t)(x >> 8), (uint8_t)(y >> 8))) return 0;
    }
    return 1;
}

/* ---- text ---- */
static const char FONT_CHARS[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!,.-:";
static uint8_t rowbuf[20];
static void text_row(const char *s)
{
    uint8_t i, n = (uint8_t)strlen(s), x0 = (uint8_t)((20u - n) >> 1);
    const char *f;
    memset(rowbuf, B_FONT, 20);
    for (i = 0; i < n; i++) {
        for (f = FONT_CHARS; *f && *f != s[i]; f++) ;
        rowbuf[x0 + i] = (uint8_t)(B_FONT + (f - FONT_CHARS));
    }
}
static void banner(const char *a, const char *b)    /* 4 rows of window at the bottom */
{
    uint8_t n, i, *w;
    for (n = 0; n < 4u; n++) {
        text_row(n == 1u ? a : (n == 2u ? b : ""));
        w = (uint8_t *)(0x9C00u + ((uint16_t)n << 5));
        for (i = 0; i < 20u; i++) vram_put(w + i, rowbuf[i]);
    }
    WY_REG = (uint8_t)(SCR_H - 32u);
}
static void banner_off(void) { WY_REG = SCR_H; }

/* ---- path finding: breadth-first distance field from the destination, per NPC ---- */
static uint8_t dist[NNPC][PCELLS];
uint8_t *bfs_q[PCELLS];                /* queue of cells (pointers into the distance field) */
uint8_t **bfs_head, **bfs_tail;
uint8_t *bfs_goal;                     /* the NPC's cell */
uint8_t bfs_stop;                      /* distance of the NPC's cell, once reached */
static uint8_t bfs_init[PCELLS];       /* 254 = wall, 255 = not reached yet */
static uint8_t bfs_pending;            /* bit per NPC */
static const int8_t NB4[4] = { -1, 1, -(int8_t)PW, (int8_t)PW };

/* ---- map ---- */
static void build_map(void)
{
    uint8_t x, y, t, h;
    uint16_t c;
    memset(solid, 1, sizeof solid);
    memset(base_tile, B_WALL, sizeof base_tile);
    for (y = 0; y < CELLS_H; y++)
        for (x = 0; x < CELLS_W; x++) {
            c = PIDX(x, y);
            switch (MAP[y][x]) {
                case '#': t = B_WALL; break;
                case ',': t = (uint8_t)(B_FLOOR + (y & 1u)); solid[c] = 0; break;
                default:                               /* scatter the grass tufts */
                    h = (uint8_t)(x * 37u + y * 101u + x * y * 13u);
                    h ^= h >> 3;
                    t = (uint8_t)(B_GROUND + (h % 7u == 0u ? 0u : (h % 5u == 1u ? 1u : 2u)));
                    solid[c] = 0; break;
            }
            base_tile[c] = t;
        }
    for (c = 0; c < PCELLS; c++) bfs_init[c] = solid[c] ? 254u : 255u;
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

static void bfs_start(uint8_t n)
{
    uint8_t *d = dist[n], *t;
    memcpy(d, bfs_init, PCELLS);
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
    cp a, #128
    jr c, 3$
    xor a, a
    ret
3$:
    ld a, (hl+)                 ; DE = next cell
    ld e, a
    ld a, (hl+)
    ld d, a
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
static void bfs_step(void)
{
    uint8_t k;
    if (bfs_cur == 0xFFu) {
        if ((LCDC_REG & LCDCF_ON) && LY_REG > 100u) return;   /* not enough of the frame left to start */
        for (k = 0; k < NNPC; k++)
            if (bfs_pending & (1u << k)) { bfs_pending &= (uint8_t)~(1u << k); bfs_start(k); break; }
        if (bfs_cur == 0xFFu) return;
    }
    if (bfs_run()) { npc[bfs_cur].path_ok = 1; bfs_cur = 0xFF; }
}


static void set_dest(uint8_t n, uint8_t x, uint8_t y)
{
    Npc *e = &npc[n];
    e->tx = x; e->ty = y;
    e->path_ok = 0; e->arrived = 0;
    bfs_pending |= (uint8_t)(1u << n);
    if (bfs_cur == n) bfs_cur = 0xFF;                  /* a search for the old destination: drop it */
}

/* ---- player ---- */
static uint8_t feet_blocked(uint8_t x, uint8_t y)  /* feet box x-4..x+3, y-3..y+2 */
{
    if (x < 4u || x > SCR_W - 4u || y < 3u || y > SCR_H - 3u) return 1;
    return cell_solid(x - 4u, y - 3u) || cell_solid(x + 3u, y - 3u)
        || cell_solid(x - 4u, y + 2u) || cell_solid(x + 3u, y + 2u);
}
static uint16_t psubx, psuby;
static void update_player(void)
{
    int8_t dx = 0, dy = 0;
    uint16_t sp;
    uint8_t n;
    if (keys & J_LEFT) dx--;
    if (keys & J_RIGHT) dx++;
    if (keys & J_UP) dy--;
    if (keys & J_DOWN) dy++;
    if (dx || dy) {
        pdir = DIR_FROM[dy + 1][dx + 1];
        if (++panim >= 14u) { panim = 0; pstep = (pstep + 1u) & 3u; }
    } else { pstep = 0; panim = 13; }
    sp = (dx && dy) ? PLAYER_DIAG : PLAYER_SPEED;
    psubx = dx ? psubx + sp : 0;
    psuby = dy ? psuby + sp : 0;
    for (n = (uint8_t)(psubx >> 8), psubx &= 0xFFu; n; n--)
        if (!feet_blocked((uint8_t)(PX(pfx) + dx), PX(pfy))) pfx += (int16_t)dx << 8;
    for (n = (uint8_t)(psuby >> 8), psuby &= 0xFFu; n; n--)
        if (!feet_blocked(PX(pfx), (uint8_t)(PX(pfy) + dy))) pfy += (int16_t)dy << 8;
}

/* ---- NPCs ---- */
static void npc_reset(void)
{
    Npc *e;
    memset(npc, 0, sizeof npc);
    e = &npc[0];                                       /* patrol, starts top right */
    e->duty = DUTY_PATROL; e->fx = (uint16_t)PATROL_X[0] << 8; e->fy = (uint16_t)PATROL_Y[0] << 8;
    e->ang = e->want = 128; e->wp = 1;
    set_dest(0, PATROL_X[1], PATROL_Y[1]);
    e = &npc[1];                                       /* guard, in the top right room */
    e->duty = DUTY_GUARD; e->fx = (uint16_t)GUARD_X << 8; e->fy = (uint16_t)GUARD_Y << 8;
    e->ang = e->want = GUARD_ANG;
    set_dest(1, GUARD_X, GUARD_Y);
}

/* Walk towards the destination along the distance field. Returns 1 on arrival. */
static uint8_t npc_walk(uint8_t n, uint16_t speed)
{
    Npc *e = &npc[n];
    uint8_t *d = dist[n];
    int16_t ddx, ddy;
    uint16_t c, best_c, m, step;
    uint8_t k, best, x = PX(e->fx), y = PX(e->fy);

    if (e->arrived) return 1;
    if (!e->path_ok) return 0;
    ddx = (int16_t)((uint16_t)e->nx << 8) - (int16_t)e->fx;
    ddy = (int16_t)((uint16_t)e->ny << 8) - (int16_t)e->fy;
    if (!ddx && !ddy) {                                /* at the next point: choose the one after */
        c = ROWP[y >> 3] + (x >> 3);
        if (d[c] == 0u) {
            if (x == e->tx && y == e->ty) { e->arrived = 1; return 1; }
            e->nx = e->tx; e->ny = e->ty;
        } else {
            static const int8_t NB8[8] = { -1, 1, -(int8_t)PW, (int8_t)PW, -(int8_t)PW - 1, -(int8_t)PW + 1, (int8_t)PW - 1, (int8_t)PW + 1 };
            best = d[c]; best_c = c;
            for (k = 0; k < 8u; k++) {
                m = c + NB8[k];
                if (d[m] >= best) continue;
                if (k >= 4u) {                         /* diagonal: no cutting wall corners */
                    int8_t ox = (k & 1u) ? 1 : -1, oy = (k < 6u) ? -(int8_t)PW : (int8_t)PW;
                    if (solid[c + ox] || solid[c + oy]) continue;
                }
                best = d[m]; best_c = m;
            }
            if (best_c == c) { e->arrived = 1; return 1; }   /* unreachable: give up here */
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
    if (++e->anim_t >= 16u) { e->anim_t = 0; e->step = (e->step + 1u) & 3u; }
    return 0;
}

static void npc_turn(Npc *e)
{
    int8_t df = (int8_t)(e->want - e->ang);
    if (df > (int8_t)TURN_RATE) e->ang += TURN_RATE;
    else if (df < -(int8_t)TURN_RATE) e->ang -= TURN_RATE;
    else e->ang = e->want;
}

static void go_attack(uint8_t n)
{
    Npc *e = &npc[n];
    e->state = NS_ATTACK;
    e->lx = PX(pfx); e->ly = PX(pfy);
    set_dest(n, e->lx, e->ly);
    e->retarget_t = 8;
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

static void go_suspicious(uint8_t n, uint8_t x, uint8_t y)
{
    Npc *e = &npc[n];
    e->state = NS_SUSPICIOUS;
    e->state_t = SEARCH_FRAMES;
    e->sx = x; e->sy = y;
    e->pause_t = 20;                                   /* a moment of "huh?" first */
    search_next(n);
}

static void back_to_duty(uint8_t n)
{
    Npc *e = &npc[n];
    uint8_t k, best = 0, x = PX(e->fx), y = PX(e->fy);
    uint16_t bd = 0xFFFFu, dd;
    e->state = NS_NORMAL;
    e->spot = 0;
    if (e->duty == DUTY_GUARD) { set_dest(n, GUARD_X, GUARD_Y); return; }
    for (k = 0; k < 4u; k++) {                         /* nearest patrol corner */
        dd = (uint16_t)ad(x, PATROL_X[k]) + ad(y, PATROL_Y[k]);
        if (dd < bd) { bd = dd; best = k; }
    }
    e->wp = best;
    set_dest(n, PATROL_X[best], PATROL_Y[best]);
}

static void npc_duty(uint8_t n)
{
    Npc *e = &npc[n];
    if (e->duty == DUTY_PATROL) {
        if (npc_walk(n, PATROL_SPEED)) {
            e->wp = (e->wp + 1u) & 3u;
            set_dest(n, PATROL_X[e->wp], PATROL_Y[e->wp]);
        }
    } else {
        if (!npc_walk(n, PATROL_SPEED)) return;
        /* at the post: sweep the view left and right, pausing at each end */
        if (e->look_t) { e->look_t--; return; }
        if (e->want == e->ang) {
            e->want = (e->want == (uint8_t)(GUARD_ANG - GUARD_SWEEP)) ? (uint8_t)(GUARD_ANG + GUARD_SWEEP)
                                                                     : (uint8_t)(GUARD_ANG - GUARD_SWEEP);
            e->look_t = 90;
        }
    }
}

static void kill_player(void)
{
    gstate = G_DEAD;
    banner("YOU WERE CAUGHT", "PRESS A TO RETRY");
}

static void update_npc(uint8_t n)
{
    Npc *e = &npc[n];
    uint8_t x = PX(e->fx), y = PX(e->fy), px = PX(pfx), py = PX(pfy);

    if (e->state == NS_DOWN) return;

    if (e->state != NS_ATTACK) {
        if (e->seen) {                                 /* being spotted: stop and stare */
            if (!((frame ^ n) & 1u) && (e->spot += 2u) >= SPOT_FRAMES) { go_attack(n); return; }
            e->lx = px; e->ly = py;
            npc_turn(e);
            return;
        }
        if (e->spot) {
            e->spot--;
            if (e->state == NS_NORMAL) go_suspicious(n, e->lx, e->ly);
        }
    }

    switch (e->state) {
    case NS_NORMAL:
        npc_duty(n);
        break;
    case NS_SUSPICIOUS:
        if (!--e->state_t) { back_to_duty(n); break; }
        if (e->pause_t) {                              /* look around */
            e->pause_t--;
            if (!(e->pause_t & 31u)) e->want = rnd();
            break;
        }
        if (npc_walk(n, SEARCH_SPEED)) { e->pause_t = 64; search_next(n); }
        break;
    case NS_ATTACK:
        if (ad(x, px) <= KILL_DIST && ad(y, py) <= KILL_DIST) { kill_player(); break; }
        if (e->seen) {                                 /* still in sight: follow */
            e->lx = px; e->ly = py;
            if (e->retarget_t) e->retarget_t--;
            else if ((e->lx >> 3) != (e->tx >> 3) || (e->ly >> 3) != (e->ty >> 3)) {
                set_dest(n, e->lx, e->ly); e->retarget_t = 8;
            }
        }
        if (npc_walk(n, ATTACK_SPEED)) {
            /* at the last known position and the player isn't here: look again */
            uint8_t ex = x, ey = (uint8_t)(y - 5u), cx = px, cy = (uint8_t)(py - 6u);
            uint16_t d2 = (uint16_t)ad(ex, cx) * ad(ex, cx) + (uint16_t)ad(ey, cy) * ad(ey, cy);
            if (d2 <= LOOK_RADIUS * LOOK_RADIUS && los(ex, ey, cx, cy)) {
                e->lx = px; e->ly = py; set_dest(n, px, py);
            } else go_suspicious(n, x, y);
        }
        break;
    }
    npc_turn(e);
}

static void try_takedown(void)
{
    uint8_t n, px = PX(pfx), py = PX(pfy), left = 0;
    for (n = 0; n < NNPC; n++) {
        Npc *e = &npc[n];
        if (e->state == NS_DOWN) continue;
        if (e->state != NS_ATTACK && ad(PX(e->fx), px) <= TAKEDOWN_DIST && ad(PX(e->fy), py) <= TAKEDOWN_DIST) {
            e->state = NS_DOWN;
            e->seen = 0;
            continue;
        }
        left++;
    }
    if (!left) { gstate = G_CLEAR; banner("AREA CLEAR!", "PRESS A TO RESTART"); }
}

/* ---- sprites ---- */
#define spr(n, X, Y, T) do { volatile struct OAM_item_t *o_ = &shadow_OAM[n]; \
    o_->y = (uint8_t)((Y) + 16); o_->x = (uint8_t)((X) + 8); o_->tile = (T); } while (0)
static void hide(uint8_t n) { shadow_OAM[n].y = 0; }

static void draw_person(uint8_t o, uint16_t fx, uint16_t fy, uint8_t row, uint8_t step, uint8_t down)
{
    uint8_t x = (uint8_t)(PX(fx) - 8u), y = (uint8_t)(PX(fy) - 13u), t;
    t = down ? T_LYING : (uint8_t)(hero_anim[row][step] << 2);
    spr(o, x, y, t);
    spr(o + 1u, (uint8_t)(x + 8u), y, (uint8_t)(t + 2u));
}

static void draw_sprites(void)
{
    uint8_t n, o;
    draw_person(0, pfx, pfy, pdir, pstep, gstate == G_DEAD);
    for (n = 0; n < NNPC; n++) {
        Npc *e = &npc[n];
        o = (uint8_t)(2u + n * 2u);
        draw_person(o, e->fx, e->fy, ROW_FROM_OCT[(uint8_t)(e->ang + 16u) >> 5],
                    e->step, e->state == NS_DOWN);
        o = (uint8_t)(6u + n);
        if (e->state == NS_ATTACK) spr(o, PX(e->fx) - 4u, PX(e->fy) - 24u, T_BANG);
        else if (e->state == NS_SUSPICIOUS || (e->spot && (e->seen ? (frame & 4u) : 1u)))
            spr(o, PX(e->fx) - 4u, PX(e->fy) - 24u, T_QUESTION);
        else hide(o);
    }
}

/* ---- cones ---- */
static void draw_cones(void)
{
    uint8_t n, on[NNPC], ex[NNPC], ey[NNPC], k[NNPC];
    uint8_t bx = (uint8_t)(PX(pfx) - 4u), by = (uint8_t)(PX(pfy) - 12u);
    for (n = 0; n < NNPC; n++) {
        Npc *e = &npc[n];
        on[n] = e->state != NS_DOWN;
        ex[n] = PX(e->fx);
        ey[n] = (uint8_t)(PX(e->fy) - 5u);
        k[n] = (uint8_t)((uint8_t)(e->ang + 4u) >> 3);
    }
    cones_update(on, ex, ey, k);
    n = (uint8_t)(frame & 1u);                        /* each NPC looks every other frame */
    npc[n].seen = gstate == G_PLAY && cone_sees(n, bx, by);
}

/* ---- setup ---- */
static void start_game(void)
{
    uint8_t k;
    DISPLAY_OFF;
    build_map();
    draw_map();
    vram_reset();
    cone_reset();
    for (k = 0; k < 40u; k++) hide(k);
    pfx = 10u << 8; pfy = 139u << 8;                   /* bottom left corner */
    pdir = DIR_FROM[0][1]; pstep = 0; psubx = psuby = 0;
    npc_reset();
    bfs_pending = 3; bfs_cur = 0xFF;
    while (bfs_pending || bfs_cur != 0xFFu) bfs_step();   /* LCD is off: runs to the end */
    npc[0].nx = PX(npc[0].fx); npc[0].ny = PX(npc[0].fy);
    npc[1].nx = PX(npc[1].fx); npc[1].ny = PX(npc[1].fy);
    banner_off();
    gstate = G_PLAY;
    frame = 0;
    SHOW_BKG; SHOW_WIN; SHOW_SPRITES;
    DISPLAY_ON;
}

static void show_title(void)
{
    static const char *const T[CELLS_H] = {
        "", "", "", "STEALTH", "", "PROTOTYPE", "", "", "PRESS START", "", "",
        "D-PAD: MOVE", "A: TAKE DOWN", "", "", "HERO ART: GIBBONGL", "CC0", "" };
    uint8_t y, k;
    DISPLAY_OFF;
    for (k = 0; k < 40u; k++) hide(k);
    for (y = 0; y < CELLS_H; y++) { text_row(T[y]); set_bkg_tiles(0, y, 20, 1, rowbuf); }
    WY_REG = SCR_H;
    gstate = G_TITLE;
    SHOW_BKG; SHOW_SPRITES;
    DISPLAY_ON;
}

void main(void)
{
    uint8_t k;
    DISPLAY_OFF;
    LCDC_REG = (uint8_t)((LCDC_REG & (uint8_t)~LCDCF_BG8000) | LCDCF_WIN9C00 | LCDCF_BG9800 | LCDCF_OBJ16);
    set_sprite_data(0, SPR_TILES, spr_tiles);
    set_bkg_data(0, BG_TILES, bg_tiles);
    BGP_REG = 0xE4u;           /* 0 white, 1 light grey (cones), 2 dark grey, 3 black */
    OBP0_REG = 0xD0u;          /* player and icons: 1 = white, 2 = light grey, 3 = black */
    OBP1_REG = 0xE4u;          /* NPCs: the same sprites one shade darker */
    WX_REG = 7; WY_REG = SCR_H;
    SPRITES_8x16;
    for (k = 2; k < 6u; k++) shadow_OAM[k].prop = S_PALETTE;
    show_title();

    while (1) {
        wait_vbl_done();
        vram_flush();
        prev_keys = keys;
        keys = joypad();
        if (gstate == G_TITLE) {
            rnd();
            if ((keys & J_START) && !(prev_keys & J_START)) start_game();
            continue;
        }
        frame++;
        if (gstate == G_PLAY) {
            update_player();
            if ((keys & J_A) && !(prev_keys & J_A)) try_takedown();
        } else if ((keys & J_A) && !(prev_keys & J_A)) { start_game(); continue; }
        draw_cones();
        if (gstate == G_PLAY)
            for (k = 0; k < NNPC; k++) update_npc(k);
        draw_sprites();
        bfs_step();
        vram_idle();
    }
}
