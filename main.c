/*
 * main.c - Top-down "Link-like" hero for GBDK-2020 (DMG target).
 *
 * D-pad : move up / down / left / right (diagonals allowed),
 *         hero turns toward the most recently pressed direction.
 * A     : fire an orb in the direction the hero is facing.
 *
 * All graphics are generated at runtime from the ASCII art below,
 * so this is a single self-contained source file - no asset files.
 *
 * Build: lcc -o hero.gb main.c
 */

#include <gb/gb.h>
#include <gb/hardware.h>
#include <stdint.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Configuration                                                       */
/* ------------------------------------------------------------------ */

#define PLAYER_SPEED   2u   /* px per frame                          */
#define PROJ_SPEED     3u   /* px per frame                          */
#define MAX_PROJ       4u   /* projectiles in flight at once         */
#define PROJ_SPR_BASE  2u   /* OAM 0,1 = hero halves, 2..5 = shots   */

#define DIR_DOWN   0u       /* also indexes the tile block per facing */
#define DIR_UP     1u
#define DIR_RIGHT  2u
#define DIR_LEFT   3u

/* VRAM tile layout. On DMG, GBDK's default puts background and sprite
 * tiles in the same memory (0x8000), so they must not share indices.
 * Sprites use tiles 0..17; the grass tile goes right after them.      */
#define NUM_SPRITE_TILES  18u
#define GRASS_TILE        NUM_SPRITE_TILES   /* tile 18 */

/* The hardware draws each sprite 8 px right / 16 px below its OAM
 * position, so screen coordinates are converted with these offsets. */
#define OAM_X(x)  ((uint8_t)((x) + 8u))
#define OAM_Y(y)  ((uint8_t)((y) + 16u))
#define HIDE_X    200u
#define HIDE_Y    200u

/* ------------------------------------------------------------------ */
/* Pixel art (one char per pixel)                                      */
/*                                                                     */
/*   '.' transparent   '-' light (skin)   '+' mid (tunic)              */
/*   '#' dark (outline, hair, eyes, boots)                              */
/*                                                                     */
/* Hero art is 16x16, drawn as two 8x16 hardware sprites side by side. */
/* ------------------------------------------------------------------ */

static const char ART_DOWN[16][17] = {
    ".....######.....",
    "...##++++++##...",
    "..#++++++++++#..",
    "..#++++++++++#..",
    "...##++++++##...",
    "..############..",
    "..#-##----##-#..",
    "..#----------#..",
    "...#--------#...",
    "..#++++++++++#..",
    ".#+#++++++++#+#.",
    ".#+#++++++++#+#.",
    "..##++++++++##..",
    "...####++####...",
    "...####..####...",
    "...####..####..."
};

static const char ART_UP[16][17] = {
    ".....######.....",
    "...##++++++##...",
    "..#++++++++++#..",
    "..#++++++++++#..",
    "..#++++++++++#..",
    "..#++++++++++#..",
    "...##########...",
    "..#++++++++++#..",
    ".#+#++++++++#+#.",
    ".#+#++++++++#+#.",
    "..##++++++++##..",
    "...#++++++++#...",
    "...#++++++++#...",
    "...####++####...",
    "...####..####...",
    "...####..####..."
};

static const char ART_RIGHT[16][17] = {
    ".....######.....",
    "...##++++++##...",
    "..#++++++++++#..",
    "..#++++++++++#..",
    "..############..",
    "..####-------#..",
    "..####-##----#..",
    "..####-------#..",
    "...###------#...",
    "..#++++++++++#..",
    ".#+#++++++++#+#.",
    ".#+#++++++++#+#.",
    "..##++++++++##..",
    "...####++####...",
    "...####..####...",
    "...####..####..."
};

static char ART_LEFT[16][17];   /* built at startup by mirroring ART_RIGHT */

static const char ART_SHOT[8][9] = {
    "..####..",
    ".######.",
    "##----##",
    "#-####-#",
    "#-####-#",
    "##----##",
    ".######.",
    "..####.."
};

static const char ART_GRASS[8][9] = {
    "........",
    "..+...+.",
    "........",
    ".+...+..",
    "........",
    "..+....+",
    "........",
    "........"
};

/* ------------------------------------------------------------------ */
/* Runtime buffers                                                     */
/* ------------------------------------------------------------------ */

/* 18 tiles: hero = 4 facings x 4 tiles (tiles 0..15),
 * tile 16 = projectile orb, tile 17 = blank (bottom half of orb sprite). */
static uint8_t sprite_tiles[NUM_SPRITE_TILES * 16u];
static uint8_t grass_tile[16];
static uint8_t bg_map[20u * 18u];

static uint8_t player_x, player_y, player_dir, prev_keys;
static uint8_t shot_x[MAX_PROJ], shot_y[MAX_PROJ];
static uint8_t shot_dir[MAX_PROJ], shot_live[MAX_PROJ];

/* ------------------------------------------------------------------ */
/* Art -> 2bpp tile conversion                                         */
/* ------------------------------------------------------------------ */

static uint8_t color_index(char c)
{
    switch (c) {
        case '#': return 3u;
        case '+': return 2u;
        case '-': return 1u;
        default:  return 0u;
    }
}

/* Encode one 8-pixel row starting at column col0 of a string row. */
static void encode_row(const char *row, uint8_t col0, uint8_t *lo, uint8_t *hi)
{
    uint8_t x, l = 0u, h = 0u;
    for (x = 0u; x < 8u; x++) {
        uint8_t c = color_index(row[col0 + x]);
        l = (uint8_t)((l << 1) | (c & 1u));
        h = (uint8_t)((h << 1) | ((c >> 1) & 1u));
    }
    *lo = l;
    *hi = h;
}

/* Build one 8x16 hardware sprite (two stacked tiles) from an 8-pixel
 * wide column of a 16x16 grid. out[0..15] = top tile, out[16..31] = bottom. */
static void build_sprite_pair(const char grid[16][17], uint8_t col0, uint8_t *out)
{
    uint8_t y;
    for (y = 0u; y < 8u; y++)
        encode_row(grid[y], col0, &out[y * 2u], &out[y * 2u + 1u]);
    for (y = 8u; y < 16u; y++)
        encode_row(grid[y], col0, &out[(y - 8u) * 2u + 16u], &out[(y - 8u) * 2u + 17u]);
}

/* Build a single 8x8 tile (background or sprite). */
static void build_tile(const char art[8][9], uint8_t *out)
{
    uint8_t y;
    for (y = 0u; y < 8u; y++)
        encode_row(art[y], 0u, &out[y * 2u], &out[y * 2u + 1u]);
}

static void mirror_grid(const char src[16][17], char dst[16][17])
{
    uint8_t y, x;
    for (y = 0u; y < 16u; y++) {
        for (x = 0u; x < 16u; x++)
            dst[y][x] = src[y][15u - x];
        dst[y][16u] = '\0';
    }
}

/* ------------------------------------------------------------------ */
/* Game logic                                                          */
/* ------------------------------------------------------------------ */

static void put_player(void)
{
    set_sprite_tile(0u, player_dir * 4u);        /* top + bottom left  */
    set_sprite_tile(1u, player_dir * 4u + 2u);   /* top + bottom right */
    move_sprite(0u, OAM_X(player_x), OAM_Y(player_y));
    move_sprite(1u, OAM_X(player_x + 8u), OAM_Y(player_y));
}

static void kill_shot(uint8_t i)
{
    shot_live[i] = 0u;
    move_sprite(PROJ_SPR_BASE + i, HIDE_X, HIDE_Y);
}

static void spawn_shot(void)
{
    uint8_t i;
    for (i = 0u; i < MAX_PROJ; i++) {
        if (shot_live[i])
            continue;

        shot_dir[i] = player_dir;
        shot_x[i] = player_x + 4u;   /* centered on the 16x16 hero */
        shot_y[i] = player_y + 4u;

        switch (player_dir) {
            case DIR_UP:    if (player_y > 8u) shot_y[i] = player_y - 8u;  break;
            case DIR_DOWN:  shot_y[i] = player_y + 16u;                    break;
            case DIR_LEFT:  if (player_x > 8u) shot_x[i] = player_x - 8u;  break;
            case DIR_RIGHT: shot_x[i] = player_x + 16u;                   break;
        }

        shot_live[i] = 1u;
        set_sprite_tile(PROJ_SPR_BASE + i, 16u);   /* orb tile + blank tile */
        move_sprite(PROJ_SPR_BASE + i, OAM_X(shot_x[i]), OAM_Y(shot_y[i]));
        return;
    }
}

static void update_shots(void)
{
    uint8_t i;
    for (i = 0u; i < MAX_PROJ; i++) {
        if (!shot_live[i])
            continue;

        switch (shot_dir[i]) {
            case DIR_UP:
                if (shot_y[i] < PROJ_SPEED + 8u) { kill_shot(i); continue; }
                shot_y[i] -= PROJ_SPEED;
                break;
            case DIR_DOWN:
                shot_y[i] += PROJ_SPEED;
                if (shot_y[i] >= 144u) { kill_shot(i); continue; }
                break;
            case DIR_LEFT:
                if (shot_x[i] < PROJ_SPEED + 8u) { kill_shot(i); continue; }
                shot_x[i] -= PROJ_SPEED;
                break;
            case DIR_RIGHT:
                shot_x[i] += PROJ_SPEED;
                if (shot_x[i] >= 160u) { kill_shot(i); continue; }
                break;
        }

        move_sprite(PROJ_SPR_BASE + i, OAM_X(shot_x[i]), OAM_Y(shot_y[i]));
    }
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

void main(void)
{
    uint8_t i, keys;

    /* Build tile data. */
    mirror_grid(ART_RIGHT, ART_LEFT);
    build_tile(ART_GRASS, grass_tile);

    build_sprite_pair(ART_DOWN,  0u, &sprite_tiles[0u]);
    build_sprite_pair(ART_DOWN,  8u, &sprite_tiles[32u]);
    build_sprite_pair(ART_UP,    0u, &sprite_tiles[64u]);
    build_sprite_pair(ART_UP,    8u, &sprite_tiles[96u]);
    build_sprite_pair(ART_RIGHT, 0u, &sprite_tiles[128u]);
    build_sprite_pair(ART_RIGHT, 8u, &sprite_tiles[160u]);
    build_sprite_pair(ART_LEFT,  0u, &sprite_tiles[192u]);
    build_sprite_pair(ART_LEFT,  8u, &sprite_tiles[224u]);
    build_tile(ART_SHOT, &sprite_tiles[256u]);   /* tile 16: orb   */
    memset(&sprite_tiles[272u], 0, 16);          /* tile 17: blank */

    /* Sprites: tiles 0..17. */
    set_sprite_data(0u, NUM_SPRITE_TILES, sprite_tiles);
    SPRITES_8x16;

    /* Background: grass everywhere, using tile 18 so it doesn't
     * collide with the sprite tiles in shared VRAM.
     * (FIX 1: the old uint8_t loop to 360 never terminated.
     *  FIX 2: grass used to sit at tile 0 and get overwritten.) */
    set_bkg_data(GRASS_TILE, 1u, grass_tile);
    memset(bg_map, GRASS_TILE, sizeof bg_map);
    set_bkg_tiles(0u, 0u, 20u, 18u, bg_map);

    player_x   = 72u;
    player_y   = 64u;
    player_dir = DIR_DOWN;
    prev_keys  = 0u;
    for (i = 0u; i < MAX_PROJ; i++) {
        shot_live[i] = 0u;
        move_sprite(PROJ_SPR_BASE + i, HIDE_X, HIDE_Y);
    }
    put_player();

    BGP_REG  = 0xE4u;   /* background: white, light, dark, black  */
    OBP0_REG = 0xE4u;   /* sprites:    transparent, light, dark, black */
    SHOW_BKG;
    SHOW_SPRITES;
    DISPLAY_ON;

    while (1u) {
        wait_vbl_done();
        keys = joypad();

        /* Turn toward the most recently pressed direction (Zelda-style). */
        if ((keys & J_UP)    && !(prev_keys & J_UP))    player_dir = DIR_UP;
        if ((keys & J_DOWN)  && !(prev_keys & J_DOWN))  player_dir = DIR_DOWN;
        if ((keys & J_LEFT)  && !(prev_keys & J_LEFT))  player_dir = DIR_LEFT;
        if ((keys & J_RIGHT) && !(prev_keys & J_RIGHT)) player_dir = DIR_RIGHT;

        /* Move; diagonals allowed. */
        if (keys & J_UP)
            player_y = (player_y > PLAYER_SPEED) ? player_y - PLAYER_SPEED : 0u;
        if (keys & J_DOWN)
            player_y = (player_y < 128u - PLAYER_SPEED) ? player_y + PLAYER_SPEED : 128u;
        if (keys & J_LEFT)
            player_x = (player_x > PLAYER_SPEED) ? player_x - PLAYER_SPEED : 0u;
        if (keys & J_RIGHT)
            player_x = (player_x < 144u - PLAYER_SPEED) ? player_x + PLAYER_SPEED : 144u;

        /* Fire on a fresh A press. */
        if ((keys & J_A) && !(prev_keys & J_A))
            spawn_shot();

        update_shots();
        put_player();

        prev_keys = keys;
    }
}
