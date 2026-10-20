/*
 * items.c - stage 3 of the port (bank 3): items, the item bar, crouching in bushes, dragging and
 * tying up bodies. Same rules as the browser prototype (web/template.html):
 *
 *   - One item slot. Walk over an item to pick it up (what you held is dropped and can't be
 *     picked up again for 4 s). Hold A for 2 s and let go: drop it.
 *   - A, empty-handed: take down a guard up close; hold A next to a body: drag it.
 *   - Shuriken: thrown straight ahead; knocks out the first NPC it hits.
 *   - Trap: set in front of you; the next NPC to step in it is knocked out.
 *   - Stone, coin: hold A and aim with the D-pad (up to 60 px), let go to throw.
 *     A stone makes NPCs within 50 px stop for 0.5 s and look at it for 4 s.
 *     A coin seen by a calm NPC: it walks over, pockets it, and walks back.
 *   - Rope: hold A next to a knocked-out NPC for 4 s: tied up for good. Too slow, it wakes up and
 *     kills you. A tied body left in a bush disappears.
 *   - B in a bush: crouch. Calm and suspicious NPCs don't see you (attackers still do) and calm
 *     ones walk right past.
 *
 * NPCs are updated every other frame, so NPC timers here count updates (2 frames).
 */
#pragma bank 3
#include <gb/gb.h>
#include <stdint.h>
#include <string.h>
#include "world.h"
#include "gfx.h"

#define NONE         0xFFu
#define DROP_LOCK    240u       /* frames */
#define DROP_HOLD    120u       /* frames: hold A this long, then let go = drop */
#define TOSS_RANGE   60u        /* px (the prototype's 40, +50%) */
#define TOSS_RANGE2  3600u
#define STONE_RADIUS 50u
#define STONE_LOOK   120u       /* updates (4 s) */
#define STONE_PAUSE  15u        /* updates (0.5 s; the prototype's 1 s, -50%) */
#define COIN_WAIT    30u        /* updates (1 s) */
#define TIE_FRAMES   240u
#define VANISH_FRAMES 45u
#define REACH_X      12u
#define REACH_Y      10u
#define SHURIKEN_STEPS 3u       /* px per frame */

Item items[NITEMS];
uint8_t n_armed, n_thrown;
static uint8_t n_items;         /* items on the ground */
uint8_t held = NONE, crouch, grab_n = NONE, tie_n = NONE, aiming;
static uint8_t a_hold, aim_moved, tie_t;
static int16_t aim_x, aim_y;
static uint16_t grab_ox, grab_oy;
static uint8_t bar_key = NONE;
static uint8_t items_ver = 1, drawn_ver, any_lock, extras;   /* extras: cross, tie bar, things in the air last frame */

/* things in the air: a shuriken (straight, 8.8 position and step) or a toss (an arc) */
typedef struct {
    uint8_t kind;               /* NONE = free */
    uint16_t x, y; int16_t dx, dy;          /* shuriken */
    uint8_t x0, y0, x1, y1, t, n, hm;       /* toss: from, to, frame, frames, arc height */
} Flying;
static Flying fly[2];

static const int8_t DIR_VEC[8][2] = { { 0, 1 }, { -1, 1 }, { -1, 0 }, { -1, -1 }, { 0, -1 }, { 1, -1 }, { 1, 0 }, { 1, 1 } };
static const uint8_t SIN32[33] = {      /* 255 * sin(pi * i / 32) */
    0, 25, 50, 74, 98, 120, 142, 162, 180, 197, 212, 225, 236, 244, 250, 254,
    255, 254, 250, 244, 236, 225, 212, 197, 180, 162, 142, 120, 98, 74, 50, 25, 0 };
static const uint8_t GLARE[8] = { 1, 2, 3, 4, 4, 3, 2, 1 };   /* star size per 3-frame step */

static uint8_t clamp8(int16_t v, uint8_t lo, uint8_t hi) { return v < lo ? lo : v > hi ? hi : (uint8_t)v; }

/* ---- items on the ground ---- */
static uint8_t land_item(uint8_t kind, int16_t cx, int16_t cy, uint8_t flags, uint8_t lock)
{
    uint8_t k;
    Item *it;
    for (k = 0, it = items; k < NITEMS && it->kind != NONE; k++, it++) ;
    if (k == NITEMS) return NONE;                       /* (can't happen: 5 items at most) */
    it->kind = kind;
    it->x = clamp8(cx - 4, 0, SCR_W - 8u);
    it->y = clamp8(cy - 4, 0, PLAY_H - 8u);
    it->lock = lock; it->flags = flags;
    if (in_bush((uint8_t)(it->x + 4u), (uint8_t)(it->y + 4u))) it->flags |= IF_BUSH;
    items_ver++; n_items++;
    if (flags & IF_ARMED) n_armed++;
    if (flags & IF_THROWN) n_thrown++;
    return k;
}
static void remove_item(Item *it)
{
    if (it->flags & IF_ARMED) n_armed--;
    if (it->flags & IF_THROWN) n_thrown--;
    it->kind = NONE; it->flags = 0;
    items_ver++; n_items--;
}
static void drop_held(void)
{
    if (held == NONE) return;
    land_item(held, PX(pfx), (int16_t)PX(pfy) - 2, 0, DROP_LOCK);   /* top left (px - 4, py - 6) */
    held = NONE;
}

static uint8_t near(uint8_t a, uint8_t b, uint8_t r) { return ad(a, b) < r; }

void items_reset(void) BANKED
{
    uint8_t k, n, tries, x, y, ok;
    memset(items, NONE, sizeof items);
    for (k = 0; k < NITEMS; k++) items[k].flags = 0;
    n_armed = n_thrown = n_items = 0;
    held = NONE; crouch = 0; grab_n = NONE; tie_n = NONE; aiming = 0;
    a_hold = NONE;                                       /* (A still held from the last screen) */
    fly[0].kind = fly[1].kind = NONE;
    bar_key = NONE; items_ver++; extras = 1;
    if (lv->nitem != 0xFFu) {
        for (k = 0; k < lv->nitem; k++) land_item(lv->item[k].kind, lv->item[k].x + 4, lv->item[k].y + 4, 0, 0);
        return;
    }
    /* random: one of each, on a free cell away from the start positions, the bushes and each other */
    for (k = 0; k < IT_KINDS; k++) {
        for (tries = 0; tries < 200u; tries++) {
            x = (uint8_t)((rnd() % CELLS_W) * 8u); y = (uint8_t)((rnd() % CELLS_H) * 8u);
            if (cell_solid(x, y)) continue;
            ok = !(near(lv->sx, x + 4u, 24u) && near(lv->sy, y + 4u, 24u));
            for (n = 0; ok && n < lv->nnpc; n++)
                if (near(lv->npc[n].x, x + 4u, 24u) && near(lv->npc[n].y, y + 4u, 24u)) ok = 0;
            for (n = 0; ok && n < lv->nbush; n++)
                if (near(lv->bush[n].x + 12u, x + 4u, 24u) && near(lv->bush[n].y + 12u, y + 4u, 24u)) ok = 0;
            for (n = 0; ok && n < NITEMS; n++)
                if (items[n].kind != NONE && near(items[n].x, x, 24u) && near(items[n].y, y, 24u)) ok = 0;
            if (ok) { land_item(k, x + 4, y + 4, 0, 0); break; }
        }
    }
}

/* ---- the player's side ---- */
static uint8_t body_in_reach(uint8_t untied_only)    /* a knocked-out NPC lying within reach */
{
    uint8_t n, px = PX(pfx), py = PX(pfy);
    Npc *e;
    for (n = 0, e = npc; n < nnpc; n++, e++)
        if (e->state == NS_DOWN && !(untied_only && e->tied)
            && ad(PX(e->fx), px) <= REACH_X && ad((uint8_t)(PX(e->fy) + LIE_DROP), py) <= REACH_Y) return n;
    return NONE;
}

static void clamp_aim(void)
{
    int16_t ox = PX(pfx), oy = (int16_t)PX(pfy) - 6, dx = aim_x - ox, dy = aim_y - oy;
    /* past the range: pull in along the longer axis, a pixel at a time (slides round the circle) */
    while ((uint16_t)(dx * dx) + (uint16_t)(dy * dy) > TOSS_RANGE2) {
        if ((dx < 0 ? -dx : dx) >= (dy < 0 ? -dy : dy)) dx += dx < 0 ? 1 : -1;
        else dy += dy < 0 ? 1 : -1;
    }
    aim_x = ox + dx; aim_y = oy + dy;
    if (aim_x < 4) aim_x = 4;
    if (aim_x > SCR_W - 5) aim_x = SCR_W - 5;
    if (aim_y < 4) aim_y = 4;
    if (aim_y > PLAY_H - 5) aim_y = PLAY_H - 5;
}
static void start_aim(void)
{
    const int8_t *v = DIR_VEC[pdir];
    aim_x = (int16_t)PX(pfx) + v[0] * 16;
    aim_y = (int16_t)PX(pfy) - 6 + v[1] * 16;
    aim_moved = 0; aiming = 1;
    clamp_aim();
}
static void aim_move(void)                              /* the D-pad moves the cross, not the player */
{
    int8_t dx = 0, dy = 0;
    if (keys & J_LEFT) dx--;
    if (keys & J_RIGHT) dx++;
    if (keys & J_UP) dy--;
    if (keys & J_DOWN) dy++;
    if (dx || dy) { aim_x += dx; aim_y += dy; aim_moved = 1; clamp_aim(); }
}
static void toss(void)
{
    uint8_t x0 = PX(pfx), y0 = (uint8_t)(PX(pfy) - 6u), x1 = (uint8_t)aim_x, y1 = (uint8_t)aim_y, i, d;
    Flying *f = fly[0].kind == NONE ? &fly[0] : &fly[1];
    for (i = 0; i < 64u && cell_solid(x1, y1); i++) {   /* landing in a wall: come back towards the player */
        if (x1 != x0) x1 += x1 < x0 ? 1 : -1;
        if (y1 != y0) y1 += y1 < y0 ? 1 : -1;
    }
    d = dist8(ad(x0, x1), ad(y0, y1));
    f->kind = held; f->x0 = x0; f->y0 = y0; f->x1 = x1; f->y1 = y1; f->t = 0;
    f->n = (uint8_t)(14u + ((d * 154u) >> 8));          /* 14 + 0.6 d (>= 12) */
    f->hm = (uint8_t)(8u + ((d * 90u) >> 8));           /* arc: 8 + 0.35 d */
    held = NONE; aiming = 0;
}

static void use_item(void)                              /* A tapped with an item */
{
    const int8_t *v = DIR_VEC[pdir];
    uint8_t px = PX(pfx), py = PX(pfy), tx, ty;
    Flying *f;
    if (held == IT_SHURIKEN) {
        f = fly[0].kind == NONE ? &fly[0] : &fly[1];
        f->kind = IT_SHURIKEN;
        f->x = (uint16_t)px << 8; f->y = (uint16_t)(py - 6u) << 8;
        f->dx = v[0] * (v[1] ? 181 : 256); f->dy = v[1] * (v[0] ? 181 : 256);
        held = NONE;
    } else if (held == IT_TRAP) {                       /* set it in front of you, unless that's a wall */
        tx = (uint8_t)(px + v[0] * 12); ty = (uint8_t)(py - 2u + v[1] * 12);
        if (tx < 4u || tx > SCR_W - 4u || ty < 4u || ty > PLAY_H - 4u) return;
        if (cell_solid(tx - 4u, ty - 4u) || cell_solid(tx + 3u, ty - 4u) || cell_solid(tx - 4u, ty + 3u) || cell_solid(tx + 3u, ty + 3u)) return;
        land_item(IT_TRAP, tx, ty, IF_ARMED, 0);
        held = NONE;
    }
}

/* NPCs near a landed stone turn to look at it for 4 s, then look back */
static void stone_landed(uint8_t x, uint8_t y)
{
    uint8_t n;
    Npc *e;
    for (n = 0, e = npc; n < nnpc; n++, e++) {
        if (e->state == NS_DOWN || e->state == NS_GONE || e->state == NS_ATTACK) continue;
        if (dist8(ad(PX(e->fx), x), ad((uint8_t)(PX(e->fy) - 5u), y)) > STONE_RADIUS) continue;
        if (!e->d_on) { e->d_prev = e->ang; e->d_pause = STONE_PAUSE; }
        else if (!e->d_pause) e->d_pause = STONE_PAUSE;   /* (a second stone doesn't restart a pause) */
        e->d_on = 1; e->d_back = 0; e->d_t = STONE_LOOK;
        e->d_ang = atan2b((int16_t)x - PX(e->fx), (int16_t)y - (int16_t)(PX(e->fy) - 5u));
        e->look = 0;
    }
}

static void update_flying(void)
{
    uint8_t i, s, nx, ny, n;
    uint16_t x, y;
    Flying *f;
    Npc *e;
    for (i = 0, f = fly; i < 2u; i++, f++) {
        if (f->kind == NONE) continue;
        if (f->kind != IT_SHURIKEN) {                   /* toss: lands after n frames */
            if (++f->t < f->n) continue;
            land_item(f->kind, f->x1, f->y1, f->kind == IT_COIN ? IF_THROWN : 0, 0);
            if (f->kind == IT_STONE) stone_landed(f->x1, f->y1);
            f->kind = NONE;
            continue;
        }
        for (s = 0; s < SHURIKEN_STEPS; s++) {          /* straight on until an NPC, a wall or the edge */
            x = f->x + f->dx; y = f->y + f->dy;
            nx = (uint8_t)(x >> 8); ny = (uint8_t)(y >> 8);
            for (n = 0, e = npc; n < nnpc; n++, e++) {
                if (e->state == NS_DOWN || e->state == NS_GONE) continue;
                if ((uint8_t)(nx - PX(e->fx) + 4u) < 8u && (uint8_t)(ny - PX(e->fy) + 13u) <= 13u) break;
            }
            if (n < nnpc) { knock_out(n); land_item(IT_SHURIKEN, PX(e->fx), (int16_t)PX(e->fy) - 2, 0, 0); break; }
            if ((x >> 8) >= SCR_W || (y >> 8) >= PLAY_H) { land_item(IT_SHURIKEN, f->x >> 8, f->y >> 8, 0, 0); break; }
            if (cell_solid(nx, ny) & OPAQUE) {          /* stop in front of the wall (it flies over the ravine) */
                land_item(IT_SHURIKEN, (int16_t)(f->x >> 8) - (f->dx >> 6), (int16_t)(f->y >> 8) - (f->dy >> 6), 0, 0);
                break;
            }
            f->x = x; f->y = y;
        }
        if (s < SHURIKEN_STEPS) f->kind = NONE;
    }
}

static void pick_up(void)                               /* walking over an item takes it */
{
    uint8_t k, bx = (uint8_t)(PX(pfx) - 4u), by = (uint8_t)(PX(pfy) - 12u), kind;
    Item *it, *got = 0;
    any_lock = 0;
    for (k = 0, it = items; k < NITEMS; k++, it++) {
        if (it->kind == NONE) continue;
        if (it->lock) { if (--it->lock) any_lock = 1; else items_ver++; continue; }
        if (got || (it->flags & IF_ARMED)) continue;
        if ((uint8_t)(it->x + 7u - bx) < 15u && (uint8_t)(it->y + 7u - by) < 20u) got = it;
    }
    if (!got) return;
    kind = got->kind;
    remove_item(got);
    drop_held();
    held = kind;
}

static uint8_t body_in_bush(const Npc *e) { return in_bush(PX(e->fx), (uint8_t)(PX(e->fy) - 6u + LIE_DROP)); }

void items_player(uint8_t pressed) BANKED
{
    uint8_t n, released = (prev_keys & J_A) && !(keys & J_A);
    Npc *e;
    if (aiming) aim_move();
    /* drag: empty-handed, holding A next to a body, it moves along with you */
    if (held != NONE || !(keys & J_A) || tie_n != NONE || !n_down) grab_n = NONE;
    else {
        if (grab_n != NONE && npc[grab_n].state != NS_DOWN) grab_n = NONE;
        if (grab_n == NONE) {
            n = body_in_reach(0);
            if (n != NONE) { grab_n = n; grab_ox = npc[n].fx - pfx; grab_oy = npc[n].fy - pfy; }
        } else {
            uint16_t tx = pfx + grab_ox, ty = pfy + grab_oy;
            e = &npc[grab_n];
            if (!feet_blocked(PX(tx), PX(e->fy))) e->fx = tx;   /* walls stop the body, axis by axis */
            if (!feet_blocked(PX(e->fx), PX(ty))) e->fy = ty;
        }
    }
    /* tie: hold A for 4 s; let go early and you keep the rope */
    if (tie_n != NONE) {
        if (!(keys & J_A)) tie_n = NONE;
        else if (!--tie_t) { npc[tie_n].tied = 1; npc[tie_n].state_t = KO_FRAMES; tie_n = NONE; held = NONE; }
    }
    /* B in a bush: crouch / stand up; leaving the bush stands you up */
    if (!lv->nbush || !in_bush(PX(pfx), (uint8_t)(PX(pfy) - 4u))) crouch = 0;
    else if (pressed & J_B) crouch ^= 1u;
    /* a tied-up body in a bush fades away and is gone for good */
    for (n = 0, e = npc; n_down && n < nnpc; n++, e++) {
        if (e->state != NS_DOWN || !e->tied || n == grab_n || !body_in_bush(e)) { e->vanish_t = 0; continue; }
        if (++e->vanish_t >= VANISH_FRAMES) { e->state = NS_GONE; e->vanish_t = 0; n_down--; }
    }
    if (n_items) pick_up();
    /* A: tap empty-handed = take down; with an item = use it; hold 2 s and let go = drop it */
    if (pressed & J_A) {
        a_hold = 0;
        if (held == IT_ROPE && (n = body_in_reach(1)) != NONE) { tie_n = n; tie_t = TIE_FRAMES; a_hold = NONE; }
        else if (held == NONE) { try_takedown(); a_hold = NONE; }
        else if (held == IT_COIN || held == IT_STONE) start_aim();
    }
    if (a_hold != NONE) {
        if ((keys & J_A) && a_hold < 250u) a_hold++;
        if (released) {
            uint8_t lng = a_hold >= DROP_HOLD;
            if (aiming) { if (lng && !aim_moved) { aiming = 0; drop_held(); } else toss(); }
            else if (held != NONE) { if (lng) drop_held(); else use_item(); }
            a_hold = 0;
        }
    }
    if (fly[0].kind != NONE || fly[1].kind != NONE) update_flying();
}

/* ---- NPCs ---- */
void npc_distract(uint8_t n) BANKED                     /* looking at where a stone landed */
{
    Npc *e = &npc[n];
    if (e->d_pause) { e->d_pause--; e->want = e->ang; }  /* hears it: stops before turning */
    else if (!e->d_back) { e->want = e->d_ang; if (!--e->d_t) { e->d_back = 1; e->want = e->d_prev; } }
    else if (e->ang == e->d_prev) e->d_on = 0;
    else e->want = e->d_prev;
    npc_turn(e);
}

uint8_t npc_see_coin(uint8_t n) BANKED                  /* a thrown coin in view: go and get it */
{
    uint8_t k;
    Item *it;
    Npc *e = &npc[n];
    for (k = 0, it = items; k < NITEMS; k++, it++) {
        if (it->kind != IT_COIN || (it->flags & (IF_THROWN | IF_CLAIMED | IF_BUSH)) != IF_THROWN) continue;
        if (cell_solid((uint8_t)(it->x + 4u), (uint8_t)(it->y + 6u))) continue;   /* (can't walk there) */
        if (!cone_sees_box(n, it->x, it->y, 8u, 8u)) continue;
        it->flags |= IF_CLAIMED;
        e->state = NS_COIN; e->look = 0;
        e->c_item = k; e->c_phase = 0; e->c_t = 0;
        e->c_ox = PX(e->fx); e->c_oy = PX(e->fy); e->c_oang = e->ang;
        set_dest(n, (uint8_t)(it->x + 4u), (uint8_t)(it->y + 6u > PLAY_H - 4u ? PLAY_H - 4u : it->y + 6u));
        return 1;
    }
    return 0;
}

/* coin errand: walk to it, wait, pocket it, wait, turn round clockwise, walk back, face the old way */
enum { C_GO, C_WAIT1, C_WAIT2, C_TURN, C_BACK, C_FACE };
void npc_coin(uint8_t n) BANKED
{
    Npc *e = &npc[n];
    Item *it = e->c_item != NONE ? &items[e->c_item] : 0;
    uint8_t df;
    switch (e->c_phase) {
    case C_GO:
        if (!it || it->kind != IT_COIN || !(it->flags & IF_CLAIMED)) {   /* someone took it */
            e->c_item = NONE; e->c_phase = C_TURN;
            e->want = atan2b((int16_t)e->c_ox - PX(e->fx), (int16_t)e->c_oy - PX(e->fy));
            break;
        }
        if (npc_walk(n, SEARCH_SPEED)) { e->c_phase = C_WAIT1; e->c_t = COIN_WAIT; }
        break;
    case C_WAIT1:
        if (--e->c_t) break;
        if (it && it->kind == IT_COIN && (it->flags & IF_CLAIMED)) remove_item(it);   /* pockets it */
        e->c_item = NONE;
        e->c_phase = C_WAIT2; e->c_t = COIN_WAIT;
        break;
    case C_WAIT2:
        if (--e->c_t) break;
        e->c_phase = C_TURN;
        e->want = atan2b((int16_t)e->c_ox - PX(e->fx), (int16_t)e->c_oy - PX(e->fy));
        break;
    case C_TURN:                                        /* clockwise only */
        df = (uint8_t)(e->want - e->ang);
        if (df <= TURN_RATE) { e->ang = e->want; e->c_phase = C_BACK; set_dest(n, e->c_ox, e->c_oy); }
        else e->ang += TURN_RATE;
        return;
    case C_BACK:
        if (npc_walk(n, PATROL_SPEED)) { e->c_phase = C_FACE; e->want = e->c_oang; }
        break;
    case C_FACE:
        if (e->ang == e->want) { resume_duty(n); return; }
        break;
    }
    npc_turn(e);
}

/* ---- sprites ---- */
#define OAM_TIE    19u
#define OAM_CROSS  20u
#define OAM_FLY    21u          /* 2 */
#define OAM_SHADOW 23u          /* 2 */
#define OAM_ITEMS  25u          /* NITEMS */
#define OAM_GLARE  35u          /* 5 */
#define OAM_END    40u

void items_draw(void) BANKED
{
    uint8_t k, x, y, h, t, *o;
    Item *it;
    Flying *f;
    /* items (only when something changed): blink while they can't be picked up; under the leaves in a bush */
    if (items_ver != drawn_ver || any_lock || n_thrown) {
        drawn_ver = items_ver;
        o = (uint8_t *)&shadow_OAM[OAM_ITEMS];
        for (k = NITEMS, it = items; k; k--, it++, o += 4) {
            if (it->kind == NONE || (it->lock && !((uint8_t)frame & 8u))) { o[0] = 0; continue; }
            o[0] = (uint8_t)(it->y + 16u); o[1] = (uint8_t)(it->x + 8u); o[2] = (uint8_t)(T_ITEM + (it->kind << 1));
            o[3] = (it->flags & IF_BUSH) ? (S_PALETTE | 0x80u) : S_PALETTE;
        }
        o = (uint8_t *)&shadow_OAM[OAM_GLARE];         /* thrown coins: a glint every 1.5 s */
        for (k = NITEMS, it = items; n_thrown && k && o != (uint8_t *)&shadow_OAM[OAM_END]; k--, it++) {
            if (it->kind != IT_COIN || !(it->flags & IF_THROWN)) continue;
            t = (uint8_t)(((uint8_t)frame + (uint8_t)(it->x * 7u + it->y * 3u)) % 90u);
            if (t >= 24u) continue;
            o[0] = (uint8_t)(it->y + 14u); o[1] = (uint8_t)(it->x + 6u);
            o[2] = (uint8_t)(T_GLARE + ((GLARE[t / 3u] - 1u) << 1)); o[3] = 0;
            o += 4;
        }
        while (o != (uint8_t *)&shadow_OAM[OAM_END]) { o[0] = 0; o += 4; }
    }
    if (!extras && !aiming && tie_n == NONE && fly[0].kind == NONE && fly[1].kind == NONE) return;
    extras = 0;
    /* in the air */
    for (k = 0, f = fly; k < 2u; k++, f++) {
        o = (uint8_t *)&shadow_OAM[OAM_FLY + k];
        if (f->kind == NONE) { o[0] = 0; shadow_OAM[OAM_SHADOW + k].y = 0; continue; }
        extras = 1;
        if (f->kind == IT_SHURIKEN) {
            o[0] = (uint8_t)((f->y >> 8) + 12u); o[1] = (uint8_t)((f->x >> 8) + 4u); o[2] = T_ITEM + IT_SHURIKEN * 2u; o[3] = S_PALETTE;
            shadow_OAM[OAM_SHADOW + k].y = 0;
            continue;
        }
        x = (uint8_t)(f->x0 + (int16_t)((int16_t)(f->x1 - f->x0) * f->t) / f->n);
        y = (uint8_t)(f->y0 + (int16_t)((int16_t)(f->y1 - f->y0) * f->t) / f->n);
        h = (uint8_t)((SIN32[(uint8_t)(((uint16_t)f->t << 5) / f->n)] * f->hm) >> 8);
        o[0] = (uint8_t)(y - h + 12u); o[1] = (uint8_t)(x + 4u); o[2] = (uint8_t)(T_ITEM + (f->kind << 1)); o[3] = S_PALETTE;
        o = (uint8_t *)&shadow_OAM[OAM_SHADOW + k];
        o[0] = (uint8_t)(y + 18u); o[1] = (uint8_t)(x + 4u); o[2] = T_SHADOW; o[3] = S_PALETTE;
    }
    /* the aiming cross */
    o = (uint8_t *)&shadow_OAM[OAM_CROSS];
    if (aiming) { o[0] = (uint8_t)(aim_y + 12); o[1] = (uint8_t)(aim_x + 4); o[2] = T_CROSS; o[3] = 0; extras = 1; }
    else o[0] = 0;
    /* tying: a bar above the player fills up over 4 s */
    o = (uint8_t *)&shadow_OAM[OAM_TIE];
    if (tie_n != NONE) {
        t = (uint8_t)((TIE_FRAMES - tie_t + 39u) / 40u);
        o[0] = (uint8_t)(PX(pfy) - 3u); o[1] = (uint8_t)(PX(pfx) + 4u); o[2] = (uint8_t)(T_BAR + t * 2u); o[3] = S_PALETTE;
        extras = 1;
    } else o[0] = 0;
}

/* ---- the item bar (window row 0): ITEM [icon] name / status ---- */
static void put_text(uint8_t col, const char *s)
{
    uint8_t c;
    for (; *s; s++, col++) {
        c = (uint8_t)*s;
        if (c >= 'A' && c <= 'Z') c = (uint8_t)(c - 'A' + 1u);
        else if (c >= '0' && c <= '9') c = (uint8_t)(c - '0' + 27u);
        else if (c == '!') c = 37u;
        else if (c == ',') c = 38u;
        else if (c == '.') c = 39u;
        else if (c == '-') c = 40u;
        else if (c == ':') c = 41u;
        else c = 0;
        rowbuf[col] = (uint8_t)(B_FONT + c);
    }
}
static const char *const ITEM_NAME[IT_KINDS] = { "SHURIKEN", "ROPE", "TRAP", "STONE", "COIN" };
enum { ST_NAME, ST_TYING, ST_DRAG, ST_HIDE, ST_DROP, ST_BLANK, ST_EMPTY };
static const char *const STATUS[] = { "", "TYING UP", "DRAGGING", "HIDING", "LET GO: DROP", "", "EMPTY" };

void items_bar(uint8_t force) BANKED
{
    uint8_t st, key;
    if (tie_n != NONE) st = ST_TYING;
    else if (held != NONE) {
        if (a_hold != NONE && a_hold >= DROP_HOLD && !(aiming && aim_moved)) st = ((uint8_t)frame & 16u) ? ST_DROP : ST_BLANK;
        else st = crouch ? ST_HIDE : ST_NAME;
    } else st = grab_n != NONE ? ST_DRAG : crouch ? ST_HIDE : ST_EMPTY;
    key = (uint8_t)((st << 3) | (held & 7u));
    if (key == bar_key && !force) return;
    bar_key = key;
    memset(rowbuf, B_FONT, 20);
    put_text(1, "ITEM");
    if (held != NONE) rowbuf[6] = (uint8_t)(B_ICON + held);
    put_text(8, st == ST_NAME ? ITEM_NAME[held] : STATUS[st]);
    win_row(0);
}
