/*
 * game.h - shared definitions for Tether, a Game Boy (DMG) game in GBDK-2020.
 *
 * A deep-sea diver collects gems on the sea floor, tethered to a water pump
 * by an air hose. The hose runs straight from the pump and catches on rocks.
 */
#ifndef GAME_H
#define GAME_H

#include <stdint.h>

#define SCR_W      160
#define PLAY_H     136          /* play area height; the HUD row is the window below */
#define CELLS_W    20u
#define CELLS_H    17u
#define NCELLS     (CELLS_W * CELLS_H)

#define MAX_PUMPS  4
#define MAX_ROCKS  10
#define MAX_GEMS   6
#define MAX_FISH   3
#define MAX_PIV    16           /* pump + caught rocks */
#define MAX_HOLES  8

/* ---- levels.c ---- */
typedef struct {
    uint8_t hose_len;           /* px */
    uint8_t nfish;
    int8_t  fish[3][2];         /* { row, speed in 1/16 px per frame (sign = direction) } */
    const char *map[8];         /* 10 x 8 grid of 16 px: S start pump, P pump, R rock, G gem */
} LevelDef;

#define NUM_LEVELS 5
extern const LevelDef levels[NUM_LEVELS];

/* ---- hose.c: the hose is drawn into a pool of background tiles ---- */
extern uint8_t base_tile[NCELLS];   /* background tile per 8x8 cell (seabed, rock, pump) */
void pool_init(void);
void hose_begin(uint8_t rebuild_fixed);
uint8_t hose_begin_cost(uint8_t rebuild_fixed);   /* scanlines, roughly */
void hose_line(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint8_t fixed);
void hose_blob(int16_t x, int16_t y);
extern uint8_t hose_ly_end;  /* hose work stops when the scanline gets near this */
uint8_t hose_commit(void);   /* 1 = done, 0 = continue next frame */
void vram_flush(void);
uint16_t vram_pending(void);
void vram_put(uint8_t *a, uint8_t v);   /* queued VRAM write, copied in the next vblank */

#endif
