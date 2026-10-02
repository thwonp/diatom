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

	case DIATOM_SCALE_ASPECT_FIT:
		a = target_aspect(src_w, src_h, aspect);
		if ((double)surf_w / a <= (double)surf_h)
			return centered(surf_w, (int)((double)surf_w / a + 0.5),
			               surf_w, surf_h);
		return centered((int)((double)surf_h * a + 0.5), surf_h,
		               surf_w, surf_h);

	case DIATOM_SCALE_STRETCH:
		return centered(surf_w, surf_h, surf_w, surf_h);
	}

	return centered(src_w, src_h, surf_w, surf_h);
}
