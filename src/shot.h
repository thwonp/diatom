/* SPDX-License-Identifier: MIT */
/* Pictures of the screen: --shot's BMP, and the screenshot hotkey's PNG
 * (plorpos-gkd.86.2). Both start from diatom_port_grab, so what is saved is
 * what was on glass - the display's size, its scaling, the shader - and
 * neither writer needs a port. */
#ifndef DIATOM_SHOT_H
#define DIATOM_SHOT_H

#include <stdbool.h>
#include <stdint.h>

/* 24-bit BMP of rgb (R,G,B rows, top first), the format --shot always had. */
bool shot_write_bmp(const char *path, const uint8_t *rgb, int w, int h);

/* Grab the screen and write it as BMP, now. */
bool shot_capture_bmp(const char *path);

/* The hotkey: grab now, on the thread that draws, and write
 * <dir>/<rom's name>-YYYYMMDD-HHMMSS.png on a thread of its own, so a game
 * does not stop for the encode. dir is made if it is missing. One at a time:
 * a press while the last is still being written is dropped, and says so.
 * False if nothing was grabbed. */
bool shot_take(const char *dir, const char *rom);

/* Until the last shot is on the card - before a process exits. */
void shot_wait(void);

/* Once per shot, on the thread that called shot_take, after its file is
 * written (or failed): true, with whether it worked and where it went, so the
 * launcher can say so on screen (plorpos-gkd.86.2). False otherwise. *path is
 * valid until the next shot_take. */
bool shot_done(bool *ok, const char **path);

#endif
