/*
 * vram.c - VRAM write queue (from Tether).
 *
 * With the LCD on, VRAM can only be written while the PPU isn't drawing. Instead of
 * waiting, writes are queued as (address, value) and copied during vblank by a tight
 * assembly loop, and in the h-blanks after the frame's work is done (vram_idle).
 */
#include <gb/gb.h>
#include <stdint.h>
#include "game.h"

qent q[QMAX];
qent *qp = q;                   /* next free entry   (globals: used by the assembly) */
qent *qh = q;                   /* next entry to copy */

/* Call right after wait_vbl_done(): copies queued writes while the LCD is in vblank
 * (lines 144-152); the rest waits for vram_idle() or the next vblank. */
void vram_flush(void) __naked
{
    __asm
    ld a, (_qh)
    ld l, a
    ld a, (_qh+1)
    ld h, a
    ld a, (_qp)
    ld c, a
    ld a, (_qp+1)
    ld b, a                     ; BC = end of queue
1$:
    ldh a, (#0x44)              ; LY: still in vblank?
    cp a, #144
    jr c, 8$
    cp a, #153
    jr nc, 8$
    .rept 4                     ; up to 4 writes per LY check
    ld a, l
    cp a, c
    jr nz, .+6                  ; (skip the high-byte check)
    ld a, h
    cp a, b
    jr z, 9$                    ; all copied
    ld a, (hl+)
    ld e, a
    ld a, (hl+)
    ld d, a
    ld a, (hl+)
    ld (de), a
    .endm
    jr 1$
8$:                             ; vblank over: keep the rest
    ld a, l
    ld (_qh), a
    ld a, h
    ld (_qh+1), a
    ret
9$:
    ld hl, #_q
    ld a, l
    ld (_qh), a
    ld (_qp), a
    ld a, h
    ld (_qh+1), a
    ld (_qp+1), a
    ret
    __endasm;
}

/* Call when the frame's work is done, instead of idling until vblank: copies queued
 * writes during the h-blanks of the remaining lines. VRAM is free in modes 0 and 2
 * (at least ~41 cycles per line), so two writes right after mode 3 ends are safe.
 * Stops at line 141 so wait_vbl_done() still catches this frame's vblank. */
void vram_idle(void) __naked
{
    __asm
    ldh a, (#0x40)              ; LCD off: nothing to wait for
    bit 7, a
    ret z
    ld a, (_qh)
    ld l, a
    ld a, (_qh+1)
    ld h, a
    ld a, (_qp)
    ld c, a
    ld a, (_qp+1)
    ld b, a
1$:
    ld a, l
    cp a, c
    jr nz, 2$
    ld a, h
    cp a, b
    jr z, 9$
2$:
    ldh a, (#0x44)
    cp a, #141
    jr nc, 8$
3$:                             ; wait for mode 3 (drawing) ...
    ldh a, (#0x41)
    and a, #3
    cp a, #3
    jr nz, 3$
4$:                             ; ... and for it to end: h-blank starts
    ldh a, (#0x41)
    and a, #3
    cp a, #3
    jr z, 4$
    ld a, (hl+)
    ld e, a
    ld a, (hl+)
    ld d, a
    ld a, (hl+)
    ld (de), a
    ld a, l
    cp a, c
    jr nz, 5$
    ld a, h
    cp a, b
    jr z, 9$
5$:
    ld a, (hl+)
    ld e, a
    ld a, (hl+)
    ld d, a
    ld a, (hl+)
    ld (de), a
    jr 1$
8$:
    ld a, l
    ld (_qh), a
    ld a, h
    ld (_qh+1), a
    ret
9$:
    ld hl, #_q
    ld a, l
    ld (_qh), a
    ld (_qp), a
    ld a, h
    ld (_qh+1), a
    ld (_qp+1), a
    ret
    __endasm;
}

uint16_t vram_pending(void) { return (uint16_t)(qp - qh); }

void vram_put(uint8_t *a, uint8_t v) { QW(a, v); }


void vram_reset(void) { qp = qh = q; }
