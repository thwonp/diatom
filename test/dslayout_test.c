/* SPDX-License-Identifier: MIT */
/* The Focus layouts' rectangles. */
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
	CHECK(f.w == 480 && f.h == 360 && s.w == 160 && s.h == 120 && f.x + f.w + s.w == 640, "H700 fills the width");
	diatom_ds_focus_rects(1600, 720, true, &f, &s);
	CHECK(f.h <= 720 && f.x + f.w + s.w <= 1600 && f.x > 0, "wide panel letterboxed");
	printf(failures ? "FAILED\n" : "all ok\n");
	return failures != 0;
}
