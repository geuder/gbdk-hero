/*
 * levels.c - the five levels.
 *
 * Map: 10 x 8 grid of 16 px squares.
 *   S start pump   P other pump   R rock   G gem   ~ row a fish patrols (just a note)
 * Every gem and pump is within hose reach of a pump (checked in the prototype).
 */
#include "game.h"

const LevelDef levels[NUM_LEVELS] = {
    /* 1 "First dive": 1 pump, 2 rocks, no fish */
    { 160, 0, { { 0, 0 }, { 0, 0 }, { 0, 0 } }, {
        "........G.",
        "....R.....",
        "..........",
        "S........G",
        "..........",
        "....R.....",
        "........G.",
        ".........." } },
    /* 2: 1 pump, 4 rocks, 1 fish */
    { 140, 1, { { 2, 7 }, { 0, 0 }, { 0, 0 } }, {
        "G.........",
        "..R...R...",
        "~~~~~~~~~~",
        "....S.....",
        "..........",
        "..R...R...",
        "..........",
        "G........G" } },
    /* 3: 2 pumps, 5 rocks, 1 fish - the right side needs pump 2 */
    { 120, 1, { { 4, 8 }, { 0, 0 }, { 0, 0 } }, {
        "....R....G",
        "..........",
        "..R....R..",
        "S.....P...",
        "~~~~~~~~~~",
        "....R.....",
        "G.......R.",
        ".....G...G" } },
    /* 4: 3 pumps, 6 rocks, 2 fish */
    { 110, 2, { { 2, 8 }, { 5, -9 }, { 0, 0 } }, {
        "G.........",
        "..R..P.R..",
        "~~~~~~~~~~",
        "....R....G",
        "S.....R...",
        "~~~~~~~~~~",
        "..R.....P.",
        "G....R...G" } },
    /* 5: 3 pumps, 8 rocks, 3 fish - pump order matters */
    { 100, 3, { { 2, 9 }, { 2, -10 }, { 5, 10 } }, {
        "G.R....R.G",
        "....P...R.",
        "~~~~~~~~~~",
        ".R..R..R..",
        "S.........",
        "~~~~~~~~~~",
        "..R...G.P.",
        "G...R....G" } },
};
