/*
 * game.h - shared definitions for the stealth prototype (Game Boy DMG, GBDK-2020).
 */
#ifndef GAME_H
#define GAME_H

#include <stdint.h>

#define SCR_W    160u
#define SCR_H    144u
#define CELLS_W  20u
#define CELLS_H  18u
/* The map is stored with a 1-cell solid border so neighbour lookups need no bounds checks. */
#define PW       22u
#define PH       20u
#define PCELLS   (PW * PH)
#define PIDX(cx, cy) ((uint16_t)((cy) + 1u) * PW + (cx) + 1u)

extern uint8_t solid[PCELLS];       /* 1 = wall (also the border) */
extern uint8_t base_tile[PCELLS];   /* background tile of each cell */
extern const uint16_t ROWP[CELLS_H];   /* PIDX(0, row) */

/* ---- vram.c: VRAM writes queued and copied during vblank / h-blank ---- */
#define QMAX 400u
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

/* ---- cone.c: view cones drawn into a pool of background tiles ---- */
#define NCONES 2u
void cone_reset(void);              /* forget all cone tiles (after the map is redrawn) */
/* Call every frame with each cone: on[n], eye (ex[n], ey[n]), facing k[n] (0..31).
 * Changed cones are redrawn over the next three frames. */
void cones_update(const uint8_t *on, const uint8_t *ex, const uint8_t *ey, const uint8_t *k);
/* 1 if cone n lights a pixel of the 8 x 13 box at (bx, by): "the NPC sees the player". */
uint8_t cone_sees(uint8_t n, uint8_t bx, uint8_t by);

#endif
