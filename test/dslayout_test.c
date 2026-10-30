/* SPDX-License-Identifier: MIT */
/* The DS layouts' rectangles: Focus, stacked Focus, Overlay (aspect, stretch). */
#include <stdio.h>

#include "diatom.h"

static int failures;
#define CHECK(c, w) do { if (c) printf("  ok   %s\n", w); else { printf("  FAIL %s\n", w); failures++; } } while (0)

int main(void)
{
	diatom_rect f, s;

	diatom_ds_focus_rects(1024, 768, true, &f, &s);
	CHECK(f.x == 0 && f.y == 96 && f.w == 768 && f.h == 576, "Brick top focus: 3x at the left, centred");
	CHECK(s.x == 768 && s.w == 256 && s.h == 192 && s.y == 96 + 576 - 192, "small bottom screen at the bottom-right corner");
	diatom_ds_focus_rects(1024, 768, false, &f, &s);
	CHECK(s.x == 768 && s.y == 96, "bottom focus: small top screen at the top-right corner");
	diatom_ds_focus_rects(640, 480, true, &f, &s);
	CHECK(f.w == 512 && f.h == 384 && s.w == 128 && s.h == 96 && f.x + f.w + s.w == 640, "RG35XX H: 2x + the 128 px left over");
	diatom_ds_focus_rects(720, 480, true, &f, &s);
	CHECK(f.w == 512 && f.h == 384 && s.w == 208 && s.h == 156 && f.x + f.w + s.w == 720, "RG SP / RG34xxSP: 2x + 208 px");
	diatom_ds_focus_rects(1600, 720, true, &f, &s);
	CHECK(f.h <= 720 && f.x + f.w + s.w <= 1600 && f.x > 0, "wide panel letterboxed");
	{
		diatom_rect t, bt;

		diatom_ds_rects(DIATOM_SCALE_DS_A, 256, 384, 1600, 1440, false, &t, &bt);
		CHECK(t.w == 1536 && t.h == 1152 && bt.w == 384 && bt.h == 288 && bt.y == 1152, "GKD Focus Top: 6x + the height left over");
		diatom_ds_rects(DIATOM_SCALE_DS_A, 256, 384, 720, 720, false, &t, &bt);
		CHECK(t.w == 720 && t.h == 540 && bt.w == 240 && bt.h == 180, "Rotate Focus Top: 3x does not fit, unchanged");
		diatom_ds_rects(DIATOM_SCALE_DS_TOP_OVERLAY, 256, 384, 720, 480, false, &t, &bt);
		CHECK(t.w == 640 && t.h == 480 && t.x == 40 && bt.w == 256 && bt.h == 192
		      && bt.x + bt.w == 680 && bt.y + bt.h == 480, "Top Overlay aspect: 4:3 fit, bottom 2/5 at the picture's corner");
		diatom_ds_rects(DIATOM_SCALE_DS_TOP_OVERLAY, 256, 384, 720, 480, true, &t, &bt);
		CHECK(t.x == 0 && t.y == 0 && t.w == 720 && t.h == 480 && bt.w == 288 && bt.h == 216
		      && bt.x + bt.w == 720 && bt.y + bt.h == 480, "Top Overlay stretch: panel filled, small 4:3 2/5 at its corner");
		diatom_ds_rects(DIATOM_SCALE_DS_BOTTOM_OVERLAY, 256, 384, 1024, 768, false, &t, &bt);
		CHECK(bt.w == 1024 && t.w == 409 && t.x + t.w == 1024 && t.y + t.h == 768, "Bottom Overlay: the top screen small");
		diatom_ds_rects(DIATOM_SCALE_DS_TOP_OVERLAY, 256, 384, 720, 720, false, &t, &bt);
		CHECK(t.w == 720 && t.h == 540 && t.y == 90 && bt.w == 288 && bt.h == 216
		      && bt.x + bt.w == 720 && bt.y + bt.h == 630, "Rotate Top Overlay aspect: small inside the picture, not below it");
		diatom_ds_rects(DIATOM_SCALE_DS_TOP_OVERLAY, 256, 384, 720, 720, true, &t, &bt);
		CHECK(t.w == 720 && t.h == 720 && t.y == 0 && bt.x + bt.w == 720 && bt.y + bt.h == 720, "Rotate Top Overlay stretch");
		diatom_ds_rects(DIATOM_SCALE_DS_TOP_OVERLAY, 256, 384, 1600, 1440, true, &t, &bt);
		CHECK(t.w == 1600 && t.h == 1440 && bt.w == 640 && bt.h == 480 && bt.x == 960 && bt.y == 960, "GKD Top Overlay stretch");
		diatom_ds_rects(DIATOM_SCALE_DS_A, 256, 384, 1600, 1440, true, &t, &bt);
		CHECK(t.w == 1536 && t.h == 1152, "Focus Top ignores stretch");
	}
	printf(failures ? "FAILED\n" : "all ok\n");
	return failures != 0;
}
