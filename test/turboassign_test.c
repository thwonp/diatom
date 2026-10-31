/* SPDX-License-Identifier: MIT */
/* Turbo Assign's decisions, frame by frame on a fake clock. ADR-0045. */
#include <stdio.h>

#include "diatom_port.h"
#include "turboassign.h"

static int failures;
static int on = -1;   /* the host of the last TA_TOGGLE */

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

	*hide = ta_choose(t, now, now & ~prev, trig, &ev, btn, &on);
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
	CHECK(frame(&t, B(A), 0, false, 20, trig, &btn, &hide) == TA_PICKED && btn == DIATOM_BTN_A,
	      "next button press picks the turbo button");
	CHECK(hide == B(A), "the choosing press is hidden");
	CHECK(frame(&t, B(A), B(A), false, 30, trig, &btn, &hide) == TA_NONE && hide == B(A),
	      "hidden for as long as it is held");
	frame(&t, 0, B(A), false, 40, trig, &btn, &hide);
	CHECK(hide == 0, "let go, the game has it back");
	CHECK(frame(&t, B(X), 0, false, 45, trig, &btn, &hide) == TA_TOGGLE
	      && btn == DIATOM_BTN_A && on == DIATOM_BTN_X, "the press after puts turbo A on X");
	CHECK(hide == B(X), "the host press is hidden too");
	frame(&t, 0, B(X), false, 48, trig, &btn, &hide);
	CHECK(frame(&t, B(A), 0, false, 50, trig, &btn, &hide) == TA_NONE && hide == 0,
	      "not armed: a press is the game's");

	/* A then A: its own turbo, as before */
	frame(&t, B(L3), 0, true, 60, trig, &btn, &hide);
	frame(&t, 0, B(L3), false, 61, trig, &btn, &hide);
	CHECK(frame(&t, B(A), 0, false, 62, trig, &btn, &hide) == TA_PICKED, "A picked");
	CHECK(frame(&t, B(A), B(A), false, 63, trig, &btn, &hide) == TA_NONE,
	      "holding the pick is not also the host");
	frame(&t, 0, B(A), false, 64, trig, &btn, &hide);
	CHECK(frame(&t, B(A), 0, false, 65, trig, &btn, &hide) == TA_TOGGLE
	      && btn == DIATOM_BTN_A && on == DIATOM_BTN_A, "A again: turbo A on A");
	frame(&t, 0, B(A), false, 66, trig, &btn, &hide);

	/* cancel after the pick */
	frame(&t, B(L3), 0, true, 70, trig, &btn, &hide);
	frame(&t, 0, B(L3), false, 71, trig, &btn, &hide);
	frame(&t, B(B), 0, false, 72, trig, &btn, &hide);
	frame(&t, 0, B(B), false, 73, trig, &btn, &hide);
	CHECK(frame(&t, B(L3), 0, true, 74, trig, &btn, &hide) == TA_CANCEL, "hotkey after the pick cancels");
	frame(&t, 0, B(L3), false, 75, trig, &btn, &hide);
	CHECK(frame(&t, B(X), 0, false, 76, trig, &btn, &hide) == TA_NONE && hide == 0,
	      "after that cancel, a press is the game's");
	CHECK(frame(&t, B(L3), 0, true, 77, trig, &btn, &hide) == TA_ARMED, "re-arm after cancel");
	frame(&t, 0, B(L3) | B(X), false, 78, trig, &btn, &hide);
	CHECK(frame(&t, B(Y), 0, false, 79, trig, &btn, &hide) == TA_PICKED && btn == DIATOM_BTN_Y,
	      "re-armed starts from the turbo button, not the old pick");
	ta_reset(&t);

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
	CHECK(frame(&t, B(UP) | B(R2), B(UP) | B(START) | B(SELECT), false, 3, trig, &btn, &hide) == TA_PICKED
	      && btn == DIATOM_BTN_R2, "still armed after them: R2");
	frame(&t, 0, B(R2), false, 4, trig, &btn, &hide);
	CHECK(frame(&t, B(START), 0, false, 5, trig, &btn, &hide) == TA_NONE && hide == 0,
	      "START is never a host either");

	/* the hotkey's own button is never the choice */
	ta_reset(&t);
	frame(&t, B(X), 0, true, 0, B(X), &btn, &hide);
	CHECK(frame(&t, B(X), B(X), true, 1, B(X), &btn, &hide) == TA_NONE, "a direct X trigger is not chosen");

	/* two at once: table order */
	ta_reset(&t);
	frame(&t, B(L3), 0, true, 0, trig, &btn, &hide);
	frame(&t, 0, B(L3), false, 1, trig, &btn, &hide);
	CHECK(frame(&t, B(Y) | B(B), 0, false, 2, trig, &btn, &hide) == TA_PICKED && btn == DIATOM_BTN_B,
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
