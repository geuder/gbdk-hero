/*
 * levels.c - the five levels, the same as in the browser prototype (web/template.html).
 *
 * Map: 20 x 17 cells of 8 px.  . ground   , floor inside   # wall (blocks walking and seeing)
 *                              o ravine (blocks walking only: you can see and throw across it)
 * NPCs: a route NPC walks its script (see R_ in game.h); a guard (route 0) stands at its post
 * and sweeps its view +- sweep around its facing.
 */
#include <stdint.h>
#include "game.h"

#define SPIN 0u                 /* R_TURN 0: all the way round */
#define BACK 128u               /* R_TURN 128: turn round */

/* ---- 1 FIRST STEPS: one guard in front of the exit ---- */
static const char *const MAP1[CELLS_H] = {
    "....................", "....................", "....................", "....................",
    "....................", "....................", "....................", "....................",
    "....................", "....................", "....................", "....................",
    "....................", "....................", "....................", "....................",
    "...................." };
static const Box BUSH1[] = { { 4, 56, 24, 24 } };
static const NpcDef NPC1[] = { { 80, 52, 64, 40, 0, 0 } };

/* ---- 2 DISTRACTION: a patrol walking left and right ---- */
static const char *const MAP2[CELLS_H] = {
    "....................", "....................", "....................", "......########......",
    "......########......", "....................", "....................", "....................",
    "....................", "....................", "....................", "....................",
    "....................", "....................", "....................", "....................",
    "...................." };
static const Box BUSH2[] = { { 120, 92, 24, 24 } };
static const uint8_t ROUTE2[] = { R_GO, 140, 68, R_TURN, SPIN, R_GO, 20, 68, R_TURN, SPIN, R_END };
static const NpcDef NPC2[] = { { 20, 68, 0, 0, ROUTE2, 0 } };

/* ---- 3 THE RAVINE (Patrick's sketch) ---- */
static const char *const MAP3[CELLS_H] = {
    "....................", "....................", "....................", "....................",
    ".....ooooooooooo....", ".....ooooooooooo....", ".....ooooooooooo....", ".....ooooooooooo....",
    ".....ooooooooooo....", ".....ooooooooooo....", ".....ooooooooooo....", ".....ooooooooooo....",
    ".....ooooooooooo....", "....................", "....................", "....................",
    "...................." };
static const uint8_t ROUTE3A[] = {     /* G1: the top half, left side first */
    R_GO, 20, 20, R_TURN, SPIN,        /* north to P1.1 */
    R_GO, 140, 20, R_TURN, SPIN,       /* east to P1.2 */
    R_GO, 140, 60, R_TURN, BACK,       /* south to P1.3, turn round */
    R_GO, 140, 20, R_TURN, SPIN,       /* north to P1.2 */
    R_GO, 20, 20, R_TURN, SPIN,        /* west to P1.1 */
    R_GO, 20, 68, R_TURN, BACK, R_END };   /* south to G1, turn round */
static const uint8_t ROUTE3B[] = {     /* G2: the bottom half, right side first */
    R_GO, 140, 116, R_TURN, SPIN,      /* south to P2.1 */
    R_GO, 20, 116, R_TURN, SPIN,       /* west to P2.2 */
    R_GO, 20, 76, R_TURN, SPIN, R_TURN, BACK,   /* north to P2.3, spin, turn round */
    R_GO, 20, 116, R_TURN, SPIN,       /* south to P2.2 */
    R_GO, 140, 116, R_TURN, SPIN,      /* east to P2.1 */
    R_GO, 140, 68, R_TURN, BACK, R_END };  /* north to G2, turn round */
static const NpcDef NPC3[] = { { 20, 68, 192, 0, ROUTE3A, 0 }, { 140, 68, 64, 0, ROUTE3B, 0 } };

/* ---- 4 GUARD ROTATION (Patrick's sketch): two facing pairs and a walker on a diamond.
 * Every guard runs this one loop, starting at a different step. Posts A (G1) and B (G2) at the
 * bottom, C (G4) and D (G5) at the top, 14 px apart: no way between without bumping into one. */
#define AX 73
#define AY 116
#define BX 87
#define CY 20
static const char *const MAP4[CELLS_H] = {
    "....................", "....................", "....................", "....................",
    "....................", "....................", "....................", "....................",
    "....................", "....................", "....................", "....................",
    "....................", "....................", "....................", "....................",
    "...................." };
static const uint8_t ROTATION[] = {
    R_GO, 96, 108, R_SIG, 0,                   /*  0: from P2 down to G2 and bump it */
    R_FREE, BX, AY, R_GO, BX, AY, R_TO, 128,   /*  2: take G2's post, face west */
    R_WAIT, 0,                                 /*  5: on B */
    R_FREE, AX, AY, R_GO, AX, AY, R_CW, 0,     /*  6: step over to G1's post, turn round clockwise */
    R_WAIT, 0,                                 /*  9: on A */
    R_CW, 128, R_GO, 12, 68, R_CCW, 224,       /* 10: turn round, to P1, 270 anticlockwise */
    R_GO, 64, 28, R_SIG, 1,                    /* 13: up to G4 and bump it */
    R_FREE, AX, CY, R_GO, AX, CY, R_TO, 0,     /* 15: take G4's post, face east */
    R_WAIT, 1,                                 /* 18: on C */
    R_FREE, BX, CY, R_GO, BX, CY, R_CCW, 128,  /* 19: step over to G5's post, turn round anticlockwise */
    R_WAIT, 1,                                 /* 22: on D */
    R_CCW, 0, R_GO, 148, 68, R_CCW, 96,        /* 23: turn round, to P2, 270 anticlockwise */
    R_END };
static const Box BUSH4[] = { { 68, 56, 24, 24 }, { 0, 0, 24, 24 }, { 136, 0, 24, 24 }, { 136, 112, 24, 24 } };
static const NpcDef NPC4[] = {
    { AX, AY, 0, 0, ROTATION, 9 },             /* G1: bottom pair, facing east */
    { BX, AY, 128, 0, ROTATION, 5 },           /* G2: bottom pair, facing west */
    { 148, 68, 96, 0, ROTATION, 0 },           /* G3: walking from P2 to G2 */
    { AX, CY, 0, 0, ROTATION, 18 },            /* G4: top pair, facing east */
    { BX, CY, 128, 0, ROTATION, 22 } };        /* G5: top pair, facing west */

/* ---- 5 THE HOUSE: the first prototype's map ---- */
static const char *const MAP5[CELLS_H] = {
    "....................", "....................", "....................", "....############....",
    "....#,,,,#,,,,,#....", "....#,,,,,,,,,,,....", "....#,,,,,,,,,,,....", "....#,,,,#,,,,,#....",
    "....#,,,,###,,##....", "....#,,,,#,,,,,#....", "....,,,,,#,,,,,#....", "....,,,,,,,,,,,#....",
    "....#,,,,,,,,,,#....", "....#,,,,#,,,,,#....", "....#######,,###....", "....................",
    "...................." };
static const Box BUSH5[] = { { 0, 0, 24, 24 }, { 136, 112, 24, 24 } };
static const uint8_t ROUTE5[] = { R_GO, 20, 12, R_GO, 20, 132, R_GO, 140, 132, R_GO, 140, 12, R_END };
static const NpcDef NPC5[] = { { 140, 12, 128, 0, ROUTE5, 0 }, { 100, 52, 128, 40, 0, 0 } };

const Level levels[NUM_LEVELS] = {
    { "FIRST STEPS",    MAP1, 80, 126, { 72, 0, 16, 10 }, 0,  1, BUSH1, 1, NPC1 },
    { "DISTRACTION",    MAP2, 12, 126, { 140, 0, 20, 10 }, 0, 1, BUSH2, 1, NPC2 },
    { "THE RAVINE",     MAP3, 80, 126, { 72, 0, 16, 10 }, 0,  0, 0,     2, NPC3 },
    { "GUARD ROTATION", MAP4, 12, 126, { 72, 0, 16, 10 }, 0,  4, BUSH4, 5, NPC4 },
    { "THE HOUSE",      MAP5, 10, 131, { 72, 0, 16, 10 }, LF_LOCKED | LF_ALLDOWN, 2, BUSH5, 2, NPC5 },
};
