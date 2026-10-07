/* SPDX-License-Identifier: MIT */
/* Turbo Assign. See turboassign.h and ADR-0045. */
#include <stddef.h>

#include "diatom_port.h"
#include "turboassign.h"

static const struct { int btn; const char *name; } assignable[] = {
	{ DIATOM_BTN_A,  "a"  }, { DIATOM_BTN_B,  "b"  },
	{ DIATOM_BTN_X,  "x"  }, { DIATOM_BTN_Y,  "y"  },
	{ DIATOM_BTN_L1, "l1" }, { DIATOM_BTN_R1, "r1" },
	{ DIATOM_BTN_L2, "l2" }, { DIATOM_BTN_R2, "r2" },
};
#define N_ASSIGNABLE (sizeof assignable / sizeof assignable[0])

uint32_t ta_assignable(void)
{
	uint32_t m = 0;
	size_t i;

	for (i = 0; i < N_ASSIGNABLE; i++) m |= DIATOM_BIT(assignable[i].btn);
	return m;
}

const char *ta_name(int btn)
{
	size_t i;

	for (i = 0; i < N_ASSIGNABLE; i++)
		if (assignable[i].btn == btn) return assignable[i].name;
	return NULL;
}

void ta_reset(turbo_assign *t)
{
	*t = (turbo_assign){ 0 };
}

uint32_t ta_choose(turbo_assign *t, uint32_t buttons, uint32_t pressed,
                   uint32_t trigger_bit, ta_event *ev, int *btn_out)
{
	*ev = TA_NONE;
	t->hide &= buttons;
	if (t->armed) {
		/* In table order, so two buttons landing on one frame always pick
		 * the same one. */
		uint32_t cand = pressed & ta_assignable() & ~trigger_bit;
		size_t i;

		for (i = 0; cand && i < N_ASSIGNABLE; i++) {
			uint32_t bit = DIATOM_BIT(assignable[i].btn);
			if (!(cand & bit)) continue;
			t->armed = false;
			t->hide |= bit;
			*ev = TA_TOGGLE;
			*btn_out = assignable[i].btn;
			break;
		}
	}
	return t->hide;
}

ta_event ta_trigger(turbo_assign *t, bool held, uint64_t now_us)
{
	ta_event ev = TA_NONE;

	if (held && !t->down) {
		t->down_us = now_us;
		t->cleared = false;
		t->armed = !t->armed;
		ev = t->armed ? TA_ARMED : TA_CANCEL;
	} else if (held && !t->cleared && now_us - t->down_us >= TA_CLEAR_US) {
		/* The hold armed it on the way down; clearing ends that too. */
		t->cleared = true;
		t->armed = false;
		ev = TA_CLEAR;
	}
	t->down = held;
	return ev;
}
