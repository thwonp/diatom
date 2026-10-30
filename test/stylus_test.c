/* SPDX-License-Identifier: MIT */
/* Stylus Mode's cursor, on a fake clock. */
#include <stdio.h>
#include <string.h>

#include "stylus.h"

static int failures;

#define CHECK(cond, what) do { \
	if (cond) printf("  ok   %s\n", what); \
	else { printf("  FAIL %s\n", what); failures++; } \
} while (0)

static uint64_t t;
static void step(unsigned d, bool tap) { t += 16667; sty_frame(d, tap, t); }

int main(void)
{
	int x, y, i;
	bool down;
	int16_t px, py;
	static uint16_t f[256 * 192];

	sty_reset();
	sty_screen(true, 256, 192);
	step(STY_RIGHT, false);
	CHECK(!sty_active(), "off until toggled");
	sty_toggle();
	step(0, false);
	CHECK(sty_cursor(&x, &y, &down) && x == 128 && y == 96 && !down, "placed in the middle");
	step(STY_RIGHT, false);
	sty_cursor(&x, &y, &down);
	CHECK(x == 129 || x == 130, "a first press moves about a pixel");
	for (i = 0; i < 6; i++) step(STY_RIGHT, false);
	sty_cursor(&x, &y, &down);
	CHECK(x > 129 && x < 150, "slow at first");
	for (i = 0; i < 120; i++) step(STY_RIGHT, false);
	sty_cursor(&x, &y, &down);
	CHECK(x == 255, "held to the edge, clamped");
	step(0, false);
	step(STY_UP | STY_LEFT, true);
	CHECK(sty_pointer(&px, &py), "tap is a press");
	sty_cursor(&x, &y, &down);
	CHECK(down, "cursor shows pressed");
	step(0, false);
	CHECK(!sty_pointer(&px, &py), "released");
	sty_screen(false, 256, 192);
	CHECK(!sty_active() && !sty_cursor(&x, &y, &down), "top screen: inactive, d-pad stays a d-pad");
	sty_screen(true, 256, 192);
	sty_cursor(&x, &y, &down);
	sty_stamp(f, 256 * 2, 2);
	CHECK(f[y * 256 + x] == 0xFFFF, "cross centre is white");
	sty_reset();
	CHECK(!sty_active(), "reset turns it off");
	CHECK(sty_source() == STY_SRC_DPAD && !strcmp(sty_source_name(), "dpad"), "the d-pad by default");
	CHECK(sty_set_source("left") && sty_source() == STY_SRC_LSTICK, "left stick");
	CHECK(sty_set_source("right") && sty_source() == STY_SRC_RSTICK, "right stick");
	CHECK(!sty_set_source("up") && sty_source() == STY_SRC_RSTICK, "a bad name changes nothing");
	sty_reset();
	CHECK(sty_source() == STY_SRC_RSTICK, "a new game keeps what the launcher chose");
	/* A stick's cursor (TortOS plorpos-6ei.1): on with no toggle, hidden
	 * after 2 s still, back on the next move. */
	{
		uint64_t t = 1000000;
		int i;

		CHECK(sty_active(), "a stick's cursor is on without Stylus Mode");
		sty_frame(STY_RIGHT, false, t);
		memset(f, 0, sizeof f);
		sty_cursor(&x, &y, &down);
		sty_stamp(f, 256 * 2, 2);
		CHECK(f[y * 256 + x] == 0xFFFF, "drawn while it moves");
		for (i = 0; i < 42; i++) sty_frame(0, false, t += 50000);   /* 2.1 s */
		memset(f, 0, sizeof f);
		sty_stamp(f, 256 * 2, 2);
		CHECK(f[y * 256 + x] == 0, "hidden after 2 s still");
		sty_frame(STY_LEFT, false, t += 16000);
		sty_cursor(&x, &y, &down);
		sty_stamp(f, 256 * 2, 2);
		CHECK(f[y * 256 + x] == 0xFFFF, "back on the next move");
	}
	sty_set_source("dpad");
	printf(failures ? "FAILED\n" : "all ok\n");
	return failures != 0;
}
