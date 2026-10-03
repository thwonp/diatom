/* SPDX-License-Identifier: MIT */
/* The GKD's GL present path (plorpos-gkd.72): the game frame through a chain
 * of libretro-style single-file GLSL shaders, into the display rect.
 *
 * SDL owns the window and the GLES context; this owns everything drawn in it.
 * Functions come from SDL_GL_GetProcAddress, so nothing links libGLESv2 and
 * the sysroot needs only SDL's own GLES headers.
 *
 * A chain of zero passes is "None": one built-in pass that copies the frame
 * into the rect, nearest or linear. Every pass but the last renders into an
 * offscreen texture; the last draws straight into the rect when its scale is
 * the screen, otherwise into a texture that one final copy scales into it. */
#ifndef DIATOM_GKD_GL_H
#define DIATOM_GKD_GL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "diatom_port.h"

#define GKDGL_MAX_PASSES DIATOM_SHADER_MAX_PASSES

typedef diatom_shader_pass gkdgl_pass;

/* After SDL_GL_CreateContext. False if a function or the built-in pass is
 * missing - then nothing here may be called. */
bool gkdgl_init(void);
void gkdgl_shutdown(void);

/* Compiles the whole chain or changes nothing; `err` says which file and why.
 * n == 0 is None. `final_linear` is how the last copy scales, when there is
 * one (a last pass with a nonzero scale). */
bool gkdgl_set_chain(const gkdgl_pass *p, int n, bool final_linear,
                     char *err, size_t errcap);

/* The frame, as the core left it. */
void gkdgl_upload(const void *src, int w, int h, size_t pitch, diatom_pixfmt fmt);

/* Clears the surface (sw x sh) and draws the last uploaded frame into dst.
 * Draws on the surface never write alpha: a Wayland surface with alpha
 * below 1 is see-through, which reads as black on the GKD, and a shader is
 * free to leave any alpha it likes. */
void gkdgl_draw(int sw, int sh, diatom_rect dst, bool none_linear);

/* Over the frame, after gkdgl_draw. Rects are top-left based, as diatom's.
 * The overlay is a BGRA image with real alpha (a notice); NULL drops it. */
void gkdgl_overlay_set(const uint8_t *bgra, int w, int h);
void gkdgl_overlay_draw(int sw, int sh, diatom_rect r);
void gkdgl_fill(int sw, int sh, diatom_rect r, uint8_t red, uint8_t green,
                uint8_t blue, uint8_t alpha);

/* The surface as drawn so far, top row first, RGBA. False on a GL error. */
bool gkdgl_read(int sw, int sh, uint8_t *rgba);

#endif
