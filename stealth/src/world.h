/*
 * world.h - game state shared between main.c (bank 0) and items.c (bank 3, stage 3:
 * items, bushes, crouching, bodies).
 */
#ifndef WORLD_H
#define WORLD_H

#include <stdint.h>
#include "game.h"

enum { G_TITLE, G_PLAY, G_DEAD, G_CLEAR };
enum { NS_NORMAL, NS_SUSPICIOUS, NS_ATTACK, NS_DOWN, NS_COIN, NS_GONE };
/* NS_COIN: walking to a thrown coin and back. NS_GONE: a tied body that vanished in a bush. */

typedef struct {
    uint16_t fx, fy;            /* feet centre, 8.8 */
    uint8_t ang, want;          /* facing now / wanted, 0..255, 0 = east, 64 = south */
    uint8_t state;
    uint16_t spot;              /* detection meter */
    uint8_t seen;               /* player in the cone this frame */
    uint16_t state_t;           /* search time left / knock-out time left */
    uint8_t tx, ty;             /* destination (px) */
    uint8_t nx, ny;             /* next point on the way (px) */
    uint8_t path_ok, arrived;
    uint8_t pause_t;
    uint8_t sx, sy;             /* search centre (px) */
    uint8_t lx, ly;             /* last known player position (px) */
    uint8_t retarget_t;
    uint8_t anim_t, step;
    uint8_t look_t;             /* guard: pause at each end of the sweep */
    int8_t look;                /* look-around offset, quarter facing units */
    uint16_t sway;              /* sway phase */
    /* duty */
    const uint8_t *route;       /* 0 = guard at a post */
    uint8_t ri;                 /* byte offset of the current step */
    uint8_t route_dest, relay, returning, walking;
    uint16_t rot_left;          /* turning: units left, 0xFFFF = not turning */
    uint8_t turn_end, wait_base, has_wait;
    uint8_t home_x, home_y;     /* where it last stood on its route */
    uint8_t post_x, post_y, face, sweep;
    uint8_t field;              /* path-finding field held, or 0xFF */
    /* stage 3 */
    uint8_t tied, vanish_t;     /* tied up for good; time in a bush while tied */
    uint8_t d_on, d_pause, d_t, d_ang, d_prev, d_back;   /* looking at a stone */
    uint8_t c_item, c_phase, c_t, c_ox, c_oy, c_oang;    /* coin errand */
    uint8_t pad[64 - 63];       /* 64 bytes: npc[n] is a shift, not a multiply */
} Npc;
extern Npc npc[NNPC];

/* ---- items ---- */
#define NITEMS     10u
#define IF_ARMED   1u           /* a set trap */
#define IF_THROWN  2u           /* a thrown coin (NPCs go for it) */
#define IF_CLAIMED 4u           /* an NPC is on its way to it */
#define IF_BUSH    8u           /* lying in a bush (hidden) */
typedef struct { uint8_t kind, x, y, lock, flags; } Item;   /* kind 0xFF = free slot; x, y top left */
extern Item items[NITEMS];
extern uint8_t n_armed;         /* armed traps lying about */
extern uint8_t n_thrown;        /* thrown coins lying about */

/* ---- player (stage 3 state) ---- */
extern uint8_t held;            /* item kind held, 0xFF none */
extern uint8_t crouch;          /* crouching in a bush */
extern uint8_t grab_n;          /* NPC being dragged, 0xFF none */
extern uint8_t tie_n;           /* NPC being tied, 0xFF none */
extern uint8_t aiming;          /* moving the throw cross: the player stands still */

/* ---- shared state and helpers (main.c, bank 0) ---- */
extern uint8_t gstate, level_no, nnpc, n_down;
extern const Level *lv;
extern uint16_t frame;
extern uint8_t keys, prev_keys;
extern uint16_t pfx, pfy;
extern uint8_t pdir;
#define PX(f) ((uint8_t)((f) >> 8))
uint8_t rnd(void);
uint8_t atan2b(int16_t dx, int16_t dy);
uint8_t ad(uint8_t a, uint8_t b);
uint8_t dist8(uint8_t a, uint8_t b);
uint8_t cell_solid(uint8_t x, uint8_t y);
uint8_t feet_blocked(uint8_t x, uint8_t y);
uint8_t in_bush(uint8_t x, uint8_t y);
void set_dest(uint8_t n, uint8_t x, uint8_t y);
uint8_t npc_walk(uint8_t n, uint16_t speed);
void npc_turn(Npc *e);
void go_suspicious(uint8_t n, uint8_t x, uint8_t y);
void resume_duty(uint8_t n);
void knock_out(uint8_t n);
void kill_player(void);
void check_clear(void);
void try_takedown(void);
void text_row(const char *s);
void win_row(uint8_t n);
extern uint8_t rowbuf[20];

/* ---- items.c (bank 3) ---- */
void items_reset(void) BANKED;              /* place the level's items */
void items_player(uint8_t pressed) BANKED;  /* per frame while playing: everything the player does with items */
void items_draw(void) BANKED;               /* sprites: items, thrown things, cross, glints, tie bar */
void items_bar(uint8_t force) BANKED;       /* the item bar (window row 0), when it changes */
void npc_coin(uint8_t n) BANKED;            /* an NPC on a coin errand, one update */
void npc_distract(uint8_t n) BANKED;        /* an NPC looking at where a stone landed, one update */
uint8_t npc_see_coin(uint8_t n) BANKED;     /* 1: saw a thrown coin and goes for it */

#define PATROL_SPEED   256u     /* 0.5 px/frame (per NPC update: 1 px) */
#define SEARCH_SPEED   320u
#define TURN_RATE      6u       /* facing units per NPC update = 3 per frame */
#define KO_FRAMES      600u     /* updates: knocked out for 20 s */
#define LIE_DROP       4u       /* a lying figure is drawn this much lower */

#endif
