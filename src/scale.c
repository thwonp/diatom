/* SPDX-License-Identifier: MIT */
/* Where the picture goes.
 *
 * ADR-0007 splits this deliberately: deciding the rect is arithmetic, so it
 * lives here once and every port behaves identically. Performing the blit is
 * hardware, so it belongs to the port. Put both in the port and the maths gets
 * duplicated per device and drifts.
 *
 * Three modes, cut from seven by ADR-0040: whole pixels, the shape the core
 * asks for, or the whole panel. The default is `stretch` (ADR-0014), chosen by
 * cycling them on a real panel rather than by argument.
 *
 * The counter-example that motivated per-device arithmetic still holds: PC
 * Engine's 256x243 at 3x is 768x729, which exceeds the Miniloong's 720 lines
 * but fits the Brick's 768. Same system, different factor per device.
 */
#include "diatom.h"

/* The set, in cycle order on device: boxed with whole pixels, boxed with the
 * shape kept, then the whole panel. Seven once - integer-vertical, fill,
 * overscale and native went in ADR-0040, after the per-console coverage and
 * crop tables showed each either matched one of these or cost a crop nobody
 * wanted.
 *
 * Geometry and filter are cycled separately (mode on the shoulders, filter on
 * A) because the question worth answering is what a given geometry looks like
 * WITH and WITHOUT blending, and a single flat list makes that comparison two
 * presses apart instead of one. */
const diatom_display_mode_info diatom_modes[] = {
	{ "integer",   DIATOM_SCALE_INTEGER,
	  "largest whole factor, letterboxed" },
	{ "aspect",    DIATOM_SCALE_ASPECT_FIT,
	  "shape the core asks for, fits inside the panel" },
	{ "stretch",   DIATOM_SCALE_STRETCH,
	  "both axes filled, shape ignored" },
	{ "ds-a",      DIATOM_SCALE_DS_A,
	  "DS: big screen full width, small one below" },
	{ "ds-a-swap", DIATOM_SCALE_DS_A_SWAP,
	  "DS: the bottom screen big, the top one small above" },
	{ "ds-b",      DIATOM_SCALE_DS_B,
	  "DS: both screens the same size, stacked" },
	{ "ds-top",    DIATOM_SCALE_DS_TOP,
	  "DS: the top screen alone, fullscreen" },
	{ "ds-bottom", DIATOM_SCALE_DS_BOTTOM,
	  "DS: the bottom screen alone, fullscreen" },
	{ "ds-topfocus", DIATOM_SCALE_DS_TOP_FOCUS,
	  "DS: the top screen big (whole multiple), the bottom one small at its right" },
	{ "ds-bottomfocus", DIATOM_SCALE_DS_BOTTOM_FOCUS,
	  "DS: the bottom screen big, the top one small at its right" },
};
const int diatom_mode_count =
	(int)(sizeof diatom_modes / sizeof diatom_modes[0]);

/* Intended display aspect. Cores report one; libretro's own rule is that a
 * value <= 0 means "assume square pixels", so base geometry decides. NES
 * content is 256x240 square-pixel but reports 4:3, because a real NES pixel
 * was wider than tall on a CRT. */
static double target_aspect(int src_w, int src_h, double aspect)
{
	if (aspect > 0.0) return aspect;
	return (double)src_w / (double)src_h;
}

static diatom_rect centered(int w, int h, int surf_w, int surf_h)
{
	diatom_rect r;
	r.w = w;
	r.h = h;
	/* Truncating division can leave an odd remainder pixel at the bottom or
	 * right rather than splitting it. Nothing sane to do about a half pixel. */
	r.x = (surf_w - w) / 2;
	r.y = (surf_h - h) / 2;
	return r;
}

diatom_rect diatom_scale_rect(diatom_scale_mode mode, int src_w, int src_h,
                              double aspect, int surf_w, int surf_h)
{
	double a;
	int f, fx, fy;

	if (src_w <= 0 || src_h <= 0 || surf_w <= 0 || surf_h <= 0) {
		diatom_rect z = { 0, 0, 0, 0 };
		return z;
	}

	switch (mode) {
	case DIATOM_SCALE_INTEGER:
		fx = surf_w / src_w;
		fy = surf_h / src_h;
		f  = fx < fy ? fx : fy;
		/* Source larger than the surface. Integer scaling cannot help, so show
		 * it 1:1 and let the port clip rather than refusing the frame outright.
		 * Reachable today: SNES hires is 512x448, and 2x would need 1024x896. */
		if (f < 1) f = 1;
		return centered(src_w * f, src_h * f, surf_w, surf_h);

	case DIATOM_SCALE_DS_A:     /* drawn as two rects (diatom_ds_rects); */
	case DIATOM_SCALE_DS_A_SWAP:
	case DIATOM_SCALE_DS_B:     /* whole, as fit, when the frame is not a DS's */
	case DIATOM_SCALE_ASPECT_FIT:
		a = target_aspect(src_w, src_h, aspect);
		if ((double)surf_w / a <= (double)surf_h)
			return centered(surf_w, (int)((double)surf_w / a + 0.5),
			               surf_w, surf_h);
		return centered((int)((double)surf_h * a + 0.5), surf_h,
		               surf_w, surf_h);

	case DIATOM_SCALE_DS_TOP:   /* one screen of the stacked frame: 4:3 */
	case DIATOM_SCALE_DS_BOTTOM:
		if (src_h == src_w * 3 / 2) { src_h /= 2; aspect = 0; }
		a = target_aspect(src_w, src_h, aspect);
		if ((double)surf_w / a <= (double)surf_h)
			return centered(surf_w, (int)((double)surf_w / a + 0.5),
			               surf_w, surf_h);
		return centered((int)((double)surf_h * a + 0.5), surf_h,
		               surf_w, surf_h);

	case DIATOM_SCALE_STRETCH:
	case DIATOM_SCALE_DS_TOP_FOCUS:     /* composed to the whole panel */
	case DIATOM_SCALE_DS_BOTTOM_FOCUS:
		return centered(surf_w, surf_h, surf_w, surf_h);
	}

	return centered(src_w, src_h, surf_w, surf_h);
}

void diatom_ds_focus_rects(int surf_w, int surf_h, bool top_focus,
                           diatom_rect *focus, diatom_rect *small)
{
	/* The big screen at the whole multiple of 256x192 nearest three quarters
	 * of the width, never so wide that no small screen fits beside it; the
	 * small one fills the width left over (TortOS plorpos-6ei.4, the user's
	 * pick). 1024x768 3x + 256, 720x480 2x + 208, 640x480 2x + 128. */
	int k = (surf_w * 3 / 4 + 128) / 256, fw, fh, sw, sh, x0;

	while (k > 1 && (k * 256 >= surf_w || k * 192 > surf_h)) k--;
	fw = k * 256;
	fh = k * 192;
	if (fw >= surf_w || fh > surf_h) {      /* under 256x192: the old thirds */
		fw = surf_w * 3 / 4;
		fh = fw * 3 / 4;
		if (fh > surf_h) { fh = surf_h; fw = fh * 4 / 3; }
	}
	sw = surf_w - fw;
	sh = sw * 3 / 4;
	if (sh > fh) { sh = fh; sw = sh * 4 / 3; }
	x0 = (surf_w - fw - sw) / 2;
	*focus = (diatom_rect){ x0, (surf_h - fh) / 2, fw, fh };
	*small = (diatom_rect){ x0 + fw, top_focus ? focus->y + fh - sh : focus->y, sw, sh };
}

bool diatom_ds_rects(diatom_scale_mode mode, int src_w, int src_h,
                     int surf_w, int surf_h, diatom_rect *top, diatom_rect *bot)
{
	int sh = src_w * 3 / 4, bw, bh, lw, lh, k;
	diatom_rect big, small;
	bool swapped = mode == DIATOM_SCALE_DS_A_SWAP;

	if (swapped) mode = DIATOM_SCALE_DS_A;
	if (mode != DIATOM_SCALE_DS_A && mode != DIATOM_SCALE_DS_B) return false;
	if (src_w <= 0 || src_h != sh * 2 || surf_w <= 0 || surf_h <= 0) return false;
	if (mode == DIATOM_SCALE_DS_A) {
		bw = surf_w;
		bh = bw * 3 / 4;
		if (bh > surf_h) { bh = surf_h; bw = bh * 4 / 3; }
		/* The nearest whole multiple of 256x192 when it fits, the small
		 * screen then filling the height left over (TortOS plorpos-6ei.4):
		 * the GKD's 1600x1440 6x + 384x288; the Rotate's 2.81x stays, as
		 * 3x is wider than its panel (the user's pick). */
		k = (bw + 128) / 256;
		if (k >= 1 && k * 256 <= surf_w && k * 192 < surf_h) { bw = k * 256; bh = k * 192; }
		lh = surf_h - bh;
		lw = lh * 4 / 3;
	} else {
		/* B: the two the same size, sharing the height (the user's pick,
		 * 2026-10-18). */
		bh = lh = surf_h / 2;
		bw = lw = bh * 4 / 3;
		if (bw > surf_w) { bw = lw = surf_w; bh = lh = bw * 3 / 4; }
	}
	if (lw > surf_w) { lw = surf_w; lh = lw * 3 / 4; }
	/* Big then small, top to bottom; swapped only changes which screen is
	 * which, so the bottom screen stays below. */
	if (!swapped) {
		big   = (diatom_rect){ (surf_w - bw) / 2, 0, bw, bh };
		small = (diatom_rect){ (surf_w - lw) / 2, bh, lw, lh };
		*top = big; *bot = small;
	} else {
		small = (diatom_rect){ (surf_w - lw) / 2, 0, lw, lh };
		big   = (diatom_rect){ (surf_w - bw) / 2, lh, bw, bh };
		*top = small; *bot = big;
	}
	return true;
}
