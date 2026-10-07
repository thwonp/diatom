/* SPDX-License-Identifier: MIT */
/* Turbo Assign's decisions, frame by frame on a fake clock. ADR-0045. */
#include <stdio.h>

#include "diatom_port.h"
#include "turboassign.h"

static int failures;

#define CHECK(cond, what) do { \
	if (cond) printf("  ok   %s\n", what); \
	else { printf("  FAIL %s\n", what); failures++; } \
} while (0)

#define B(x) DIATOM_BIT(DIATOM_BTN_##x)

/* One frame: buttons held now and at the last frame, the hotkey held or not.
 * Returns the event of whichever half spoke. */
static ta_event frame(turbo_assign *t, uint32_t now, uint32_t prev, bool hk,
                      uint64_t us, uint32_t trig, int *btn, uint32_t *hide)
{
	ta_event ev, ev2;

	*hide = ta_choose(t, now, now & ~prev, trig, &ev, btn);
	ev2 = ta_trigger(t, hk, us);
	return ev != TA_NONE ? ev : ev2;
}

int main(void)
{
	turbo_assign t;
	int btn = -1;
	uint32_t hide;
	const uint32_t trig = B(L3);

	ta_reset(&t);
	CHECK(frame(&t, B(L3), 0, true, 0, trig, &btn, &hide) == TA_ARMED, "hotkey press arms");
	CHECK(frame(&t, 0, B(L3), false, 10, trig, &btn, &hide) == TA_NONE, "release does nothing");
	CHECK(frame(&t, B(A), 0, false, 20, trig, &btn, &hide) == TA_TOGGLE && btn == DIATOM_BTN_A,
	      "next button press toggles it");
	CHECK(hide == B(A), "the choosing press is hidden");
	CHECK(frame(&t, B(A), B(A), false, 30, trig, &btn, &hide) == TA_NONE && hide == B(A),
	      "hidden for as long as it is held");
	frame(&t, 0, B(A), false, 40, trig, &btn, &hide);
	CHECK(hide == 0, "let go, the game has it back");
	CHECK(frame(&t, B(A), 0, false, 50, trig, &btn, &hide) == TA_NONE && hide == 0,
	      "not armed: a press is the game's");

	/* cancel */
	frame(&t, B(L3), 0, true, 100, trig, &btn, &hide);
	frame(&t, 0, B(L3), false, 110, trig, &btn, &hide);
	CHECK(frame(&t, B(L3), 0, true, 120, trig, &btn, &hide) == TA_CANCEL, "hotkey again cancels");
	frame(&t, 0, B(L3), false, 130, trig, &btn, &hide);
	CHECK(frame(&t, B(B), 0, false, 140, trig, &btn, &hide) == TA_NONE, "after cancel, a press is the game's");

	/* never a direction, START or SELECT */
	ta_reset(&t);
	frame(&t, B(L3), 0, true, 0, trig, &btn, &hide);
	frame(&t, 0, B(L3), false, 1, trig, &btn, &hide);
	CHECK(frame(&t, B(UP) | B(START) | B(SELECT), 0, false, 2, trig, &btn, &hide) == TA_NONE && hide == 0,
	      "d-pad, START, SELECT are not assignable");
	CHECK(frame(&t, B(UP) | B(R2), B(UP) | B(START) | B(SELECT), false, 3, trig, &btn, &hide) == TA_TOGGLE
	      && btn == DIATOM_BTN_R2, "still armed after them: R2");

	/* the hotkey's own button is never the choice */
	ta_reset(&t);
	frame(&t, B(X), 0, true, 0, B(X), &btn, &hide);
	CHECK(frame(&t, B(X), B(X), true, 1, B(X), &btn, &hide) == TA_NONE, "a direct X trigger is not chosen");

	/* two at once: table order */
	ta_reset(&t);
	frame(&t, B(L3), 0, true, 0, trig, &btn, &hide);
	frame(&t, 0, B(L3), false, 1, trig, &btn, &hide);
	CHECK(frame(&t, B(Y) | B(B), 0, false, 2, trig, &btn, &hide) == TA_TOGGLE && btn == DIATOM_BTN_B,
	      "two in one frame: B before Y");

	/* hold to clear */
	ta_reset(&t);
	CHECK(frame(&t, B(L3), 0, true, 1000, trig, &btn, &hide) == TA_ARMED, "hold begins by arming");
	CHECK(frame(&t, B(L3), B(L3), true, 1000 + TA_CLEAR_US - 1, trig, &btn, &hide) == TA_NONE,
	      "not yet at 2 s");
	CHECK(frame(&t, B(L3), B(L3), true, 1000 + TA_CLEAR_US, trig, &btn, &hide) == TA_CLEAR,
	      "2 s clears");
	CHECK(!t.armed, "clearing disarms");
	CHECK(frame(&t, B(L3), B(L3), true, 1000 + 2 * TA_CLEAR_US, trig, &btn, &hide) == TA_NONE,
	      "one clear per hold");
	frame(&t, 0, B(L3), false, 1000 + 2 * TA_CLEAR_US + 1, trig, &btn, &hide);
	CHECK(frame(&t, B(A), 0, false, 1000 + 2 * TA_CLEAR_US + 2, trig, &btn, &hide) == TA_NONE,
	      "after a clear, a press is the game's");

	/* names */
	CHECK(ta_name(DIATOM_BTN_L2) && ta_name(DIATOM_BTN_START) == NULL, "names: l2 yes, start no");

	if (failures) {
		printf("\n%d check(s) failed\n", failures);
		return 1;
	}
	printf("\nok: every check passed\n");
	return 0;
}
