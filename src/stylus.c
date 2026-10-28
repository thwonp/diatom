/* SPDX-License-Identifier: MIT */
/* See stylus.h. */
#include "stylus.h"

/* Slow to start, so a menu item can be aimed at; the GKD's full-tilt speed
 * (the screen's width in 0.8 s) after RAMP_S held. */
#define CROSS_S 0.8f
#define RAMP_S  0.6f
#define SLOW    0.10f

static bool     g_mode, g_bottom, g_tap, g_placed;
static int      g_w, g_h;
static float    g_x, g_y, g_held;
static uint64_t g_last_us;

void sty_reset(void)
{
	g_mode = g_tap = g_placed = false;
	g_held = 0;
	g_last_us = 0;
}

void sty_toggle(void) { g_mode = !g_mode; }

void sty_screen(bool bottom_shown, int w, int h)
{
	g_bottom = bottom_shown;
	g_w = w;
	g_h = h;
	if (g_placed && (g_x > w - 1 || g_y > h - 1)) g_placed = false;
}

bool sty_active(void) { return g_mode && g_bottom && g_w > 0 && g_h > 0; }

static float clampf(float v, float hi) { return v < 0 ? 0 : v > hi ? hi : v; }

void sty_frame(unsigned dirs, bool tap, uint64_t now_us)
{
	float dt = g_last_us ? (now_us - g_last_us) / 1e6f : 0;
	int dx = !!(dirs & STY_RIGHT) - !!(dirs & STY_LEFT);
	int dy = !!(dirs & STY_DOWN) - !!(dirs & STY_UP);

	g_last_us = now_us;
	g_tap = tap;
	if (!sty_active()) { g_held = 0; return; }
	if (!g_placed) { g_x = g_w / 2.0f; g_y = g_h / 2.0f; g_placed = true; }
	if (!dx && !dy) { g_held = 0; return; }
	if (dt > 0.05f) dt = 0.05f;             /* back from a pause: no jump */
	if (g_held == 0) {                      /* a tap moves at least a pixel */
		g_x = clampf(g_x + dx, g_w - 1);
		g_y = clampf(g_y + dy, g_h - 1);
	}
	g_held += dt;
	{
		float n = g_held >= RAMP_S ? 1 : g_held / RAMP_S;
		float v = g_w / CROSS_S * (SLOW + (1 - SLOW) * n * n) * dt;
		float k = dx && dy ? 0.70710678f : 1;   /* diagonals no faster */

		g_x = clampf(g_x + dx * v * k, g_w - 1);
		g_y = clampf(g_y + dy * v * k, g_h - 1);
	}
}

bool sty_cursor(int *x, int *y, bool *down)
{
	if (!sty_active() || !g_placed) return false;
	*x = (int)(g_x + 0.5f);
	*y = (int)(g_y + 0.5f);
	*down = g_tap;
	return true;
}

bool sty_pointer(int16_t *x, int16_t *y)
{
	if (!sty_active() || !g_placed) return false;
	*x = (int16_t)((g_x / g_w * 2 - 1) * 0x7fff);
	*y = (int16_t)(g_y / g_h * 0x7fff);
	return g_tap;
}

static void put(void *f, size_t pitch, int bpp, int x, int y, uint8_t r, uint8_t g, uint8_t b)
{
	uint8_t *p;

	if (x < 0 || y < 0 || x >= g_w || y >= g_h) return;
	p = (uint8_t *)f + (size_t)y * pitch + (size_t)x * bpp;
	if (bpp == 2)
		*(uint16_t *)(void *)p = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
	else
		*(uint32_t *)(void *)p = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

/* A cross, dark-edged so it reads on any picture; yellow while pressed. */
void sty_stamp(void *frame, size_t pitch, int bpp)
{
	int cx, cy, i, e, t;
	bool down;

	if (!sty_cursor(&cx, &cy, &down) || (bpp != 2 && bpp != 4)) return;
	for (e = 1; e >= 0; e--)
		for (i = -6 - e; i <= 6 + e; i++) {
			uint8_t r = e ? 0 : 255, g = e ? 0 : 255, b = e ? 0 : (down ? 0 : 255);

			for (t = -e; t <= e; t++) {
				put(frame, pitch, bpp, cx + i, cy + t, r, g, b);
				put(frame, pitch, bpp, cx + t, cy + i, r, g, b);
			}
		}
}
