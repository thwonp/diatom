/* SPDX-License-Identifier: MIT */
/* The DS stylus on a pad with no stick and no panel (TortOS plorpos-reo.21):
 * Stylus Mode turns the d-pad into a cursor over the DS's bottom screen,
 * shown fullscreen on its own; Stylus Tap (HK_STYLUS) holds it down. The GKD
 * has a stick and draws its cursor in its port (gkd.c); this is for the ports
 * that show one DS screen at a time - the cursor is stamped into the cropped
 * frame, so no port draws anything.
 *
 * Positions are in the 256x192 bottom screen's own pixels. No SDL, no port:
 * a test drives it with a fake clock. */
#ifndef DIATOM_STYLUS_H
#define DIATOM_STYLUS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define STY_UP    1u
#define STY_DOWN  2u
#define STY_LEFT  4u
#define STY_RIGHT 8u

/* A new game: mode off, cursor unplaced. */
void sty_reset(void);
void sty_toggle(void);
/* Whether the bottom screen is the one shown, and its size in pixels. Stylus
 * Mode only acts then: the d-pad stays a d-pad on the top screen, where there
 * is no cursor to see. */
void sty_screen(bool bottom_shown, int w, int h);
bool sty_active(void);
/* Every frame: the d-pad directions held (STY_*), the Tap held, time in us. */
void sty_frame(unsigned dirs, bool tap, uint64_t now_us);
/* True while Tap is held (and active); x, y are the libretro pointer's. */
bool sty_pointer(int16_t *x, int16_t *y);
/* Cursor placed, in screen pixels. */
bool sty_cursor(int *x, int *y, bool *down);

/* The cursor's cross stamped on a cropped frame of 2 or 4 bytes a pixel
 * (RGB565 or XRGB8888). */
void sty_stamp(void *frame, size_t pitch, int bytes_pp);

#endif
