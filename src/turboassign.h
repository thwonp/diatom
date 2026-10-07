/* SPDX-License-Identifier: MIT */
/* Turbo Assign (plorpos-tkh, ADR-0045): the hotkey that makes a button turbo
 * for the rest of the game. Press the hotkey, then a button: that button's
 * turbo flips. The hotkey again before a button cancels; held for
 * TA_CLEAR_US it clears every turbo button.
 *
 * This decides only WHAT was asked. The pulse is ADR-0028's, in env.c, and the
 * launcher owns the map that drives it: Diatom reports the request (TURBO)
 * and the launcher answers with a SETMAP. Nothing here is persisted.
 *
 * No SDL, no port, no core - so a test drives it with fake frames and a fake
 * clock, the way rewind.c's ring is tested. */
#ifndef DIATOM_TURBOASSIGN_H
#define DIATOM_TURBOASSIGN_H

#include <stdbool.h>
#include <stdint.h>

#define TA_CLEAR_US (3u * 1000000u)

typedef enum { TA_NONE, TA_ARMED, TA_CANCEL, TA_TOGGLE, TA_CLEAR } ta_event;

typedef struct {
	bool     armed;       /* waiting for the button to toggle */
	bool     down;        /* the hotkey was held at the last step */
	bool     cleared;     /* this hold already cleared; nothing more from it */
	uint64_t down_us;     /* when the hotkey went down */
	uint32_t hide;        /* a chosen button, hidden from the game until let go */
} turbo_assign;

/* Every button that may be made turbo: the face buttons and the shoulders.
 * Never a direction, START, SELECT or a Diatom-own key. */
uint32_t ta_assignable(void);

/* The canonical name of an assignable button ("a", "l2"), or NULL. */
const char *ta_name(int btn);

void ta_reset(turbo_assign *t);

/* Once a frame, before the hotkey dispatch: if armed, the first assignable
 * button pressed this frame (other than the hotkey's own, `trigger_bit`) is
 * the choice - TA_TOGGLE with *btn_out set. Returns the bits the hotkey
 * dispatch and the game must not see this frame: the choice, for as long as
 * it stays held. */
uint32_t ta_choose(turbo_assign *t, uint32_t buttons, uint32_t pressed,
                   uint32_t trigger_bit, ta_event *ev, int *btn_out);

/* Once a frame, after the hotkey dispatch: whether the Turbo Assign binding
 * is held on its active layer this frame. */
ta_event ta_trigger(turbo_assign *t, bool held, uint64_t now_us);

#endif
