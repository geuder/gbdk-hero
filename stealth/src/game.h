/*
 * game.h - shared definitions for the stealth game (Game Boy DMG, GBDK-2020).
 */
#ifndef GAME_H
#define GAME_H

#include <stdint.h>
#include <gb/gb.h>

#define SCR_W    160u
#define SCR_H    144u
#define PLAY_H   136u            /* play area; the bottom 8 px row is the item bar (window) */
#define CELLS_W  20u
#define CELLS_H  17u
/* The map is stored with a 1-cell border so neighbour lookups need no bounds checks. */
#define PW       22u
#define PH       19u
#define PCELLS   (PW * PH)
#define PIDX(cx, cy) ((uint16_t)((cy) + 1u) * PW + (cx) + 1u)

/* solid[]: 0 = open, else bits: 1 = blocks walking, 2 = blocks the view (OPAQUE).
 * Walls are 3, the ravine is 1 (you can see and throw across it), the border is 3. */
#define OPAQUE   2u
extern uint8_t solid[PCELLS];
extern uint8_t base_tile[PCELLS];   /* background tile of each cell */
extern const uint16_t ROWP[CELLS_H + 1u];   /* PIDX(0, row) */

/* ---- vram.c: VRAM writes queued and copied during vblank / h-blank ---- */
#define QMAX 320u
typedef struct { uint8_t *a; uint8_t v; } qent;
extern qent q[QMAX];
extern qent *qp;
/* queue a VRAM write; if the queue is full, write now (slow but safe) */
#define QW(addr, val) do { if (qp != q + QMAX) { qp->a = (addr); qp->v = (val); qp++; } \
                           else set_vram_byte((addr), (val)); } while (0)
void vram_flush(void);              /* call right after wait_vbl_done() */
void vram_idle(void);               /* call when the frame's work is done */
void vram_put(uint8_t *a, uint8_t v);
uint16_t vram_pending(void);
void vram_reset(void);

/* ---- cone.c: view cones, drawn by swapping background tiles per 4 x 4 px quarter-cell ---- */
#define NCONES 5u
extern uint8_t los_open;            /* set when the level has no walls */
void cone_reset(void) BANKED;              /* forget all cone tiles (after the map is redrawn) */
/* Call every frame with each cone: on[n], eye (ex[n], ey[n]), facing k[n] (0..CONE_ANGLES-1).
 * Changed cones are redrawn over the next frames, stopping each frame when the frame position
 * (scan lines since vblank started, 0..153) passes lylimit. */
void cones_update(const uint8_t *on, const uint8_t *ex, const uint8_t *ey, const uint8_t *k, uint8_t lylimit) BANKED;
/* 1 if cone n lights a pixel of the 8 x 13 box at (bx, by): "the NPC sees the player". */
uint8_t cone_sees(uint8_t n, uint8_t bx, uint8_t by) BANKED;
uint8_t cone_sees_box(uint8_t n, uint8_t bx, uint8_t by, uint8_t w, uint8_t h) BANKED;

/* ---- levels.c ---- */
typedef struct { uint8_t x, y, w, h; } Box;
typedef struct { uint8_t x, y, face, sweep; const uint8_t *route; uint8_t step; } NpcDef;
typedef struct { uint8_t kind, x, y; } ItemDef;   /* kind: IT_ below; x, y: top left of the 8 x 8 item */
enum { IT_SHURIKEN, IT_ROPE, IT_TRAP, IT_STONE, IT_COIN, IT_KINDS };
typedef struct {
    const char *name;
    const char *const *map;         /* CELLS_H rows of CELLS_W: . ground , floor # wall o ravine */
    uint8_t sx, sy;                 /* player spawn (feet) */
    Box exit;
    uint8_t flags;                  /* LF_ below */
    uint8_t nbush; const Box *bush;
    uint8_t nnpc; const NpcDef *npc;
    uint8_t nitem; const ItemDef *item;   /* nitem 0xFF: one of each kind at random spots */
} Level;
#define LF_LOCKED   1u              /* exit locked (opens with a hostage, later) */
#define LF_ALLDOWN  2u              /* clear the level when every NPC is down */
#define NUM_LEVELS  5u
#define NNPC        5u
extern const Level levels[NUM_LEVELS];

/* route steps (bytes): a route repeats from the start after R_END */
enum { R_END, R_GO, R_TURN, R_CW, R_CCW, R_TO, R_SIG, R_WAIT, R_FREE };
/* R_GO x y     walk there (feet, px)
 * R_TURN n     turn clockwise on the spot by n (0 = all the way round)
 * R_CW a       turn clockwise until facing a;  R_CCW a  anticlockwise
 * R_TO a       turn the short way to facing a
 * R_SIG s      send bump signal s;  R_WAIT s  wait for the next bump s
 * R_FREE x y   wait until nobody stands there
 * facings: 0 east, 64 south, 128 west, 192 north */

#endif
