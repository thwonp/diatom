/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* NextUI-derived: PolyForm Noncommercial 1.0.0, NOT this repo's root MIT
 * license - see NOTICE and THIRD-PARTY.md before touching this file. */
/* Concept ported from NextUI's ma_rewind.c (PolyForm Noncommercial 1.0.0) -
 * a real ring buffer of savestate snapshots, stepped backward one entry per
 * displayed frame while held, not merely reloading the last save. See
 * THIRD-PARTY.md and src/rewind.c for what differs in the port.
 *
 * No SDL, no port header - links the way save.c and db.c-style modules do,
 * so it can be exercised with a fake diatom_core and no display. */
#ifndef DIATOM_REWIND_H
#define DIATOM_REWIND_H

#include <stddef.h>

#include "diatom.h"

/* Once every this many forward frames, not every frame - see rewind.c for
 * why. Exposed so a test can drive diatom_rewind_capture() an exact number
 * of times per logical step instead of guessing at rewind.c's cadence.
 * Rewind plays one snapshot per displayed frame, so this is also the rewind
 * speed: 15 = 15x. A port may override it from the Makefile (the GKD uses 5,
 * 5x - plorpos-gkd.24). */
#ifndef DIATOM_REWIND_CAPTURE_EVERY
#define DIATOM_REWIND_CAPTURE_EVERY 15
#endif

/* The player's rewind speed (plorpos-gkd.40): the cadence above becomes
 * this session's default, and SETREWINDSPEED replaces it. 0 turns rewind
 * off - nothing is captured, the ring is emptied and its memory freed.
 * Takes effect on the next capture; the ring's depth depends on the budget
 * and the state size only, so nothing is rebuilt. Every RUN (reset, above)
 * goes back to the default, and the launcher re-sends the player's choice
 * after it, the way it re-sends SETHOTKEYS. False, unchanged, when `every`
 * is above DIATOM_REWIND_MAX_EVERY. */
#define DIATOM_REWIND_MAX_EVERY 60
bool     diatom_rewind_set_every(int every);
unsigned diatom_rewind_every(void);

/* Called once per RUN, before the frame loop starts. Frees any previous
 * session's history and buffers (waiting for the worker thread, so never
 * from the frame loop). Nothing is sized here: buffers are built by the
 * first capture, against that capture's serialize_size(). A core that
 * reports 0 (no savestate support) never captures - rewind is simply
 * unavailable that session. */
void diatom_rewind_reset(diatom_core *c);

/* Call once per forward frame (every DIATOM_REWIND_CAPTURE_EVERY'th call
 * captures) - NOT while rewind is active. Costs the frame loop the core's
 * serialize and nothing else: compression happens on a worker thread, and
 * a capture that finds the worker behind is dropped rather than waited for.
 * Re-checks serialize_size() every capture - a core's state size can change
 * mid-session (measured on mGBA, see save.c) - and a change starts the
 * history over. */
void diatom_rewind_capture(diatom_core *c);

/* Steps one entry backward: restores the next older captured state via
 * unserialize and consumes it from the ring. The first step of a rewind
 * discards captures the worker has not committed yet instead of waiting
 * for them. False means the ring is exhausted - the caller should hold the
 * current frame rather than call this again, matching "rewind stops at the
 * oldest recorded moment" rather than wrapping around to imagined future
 * frames. */
bool diatom_rewind_step_back(diatom_core *c);

/* How many steps remain before diatom_rewind_step_back would return false.
 * For the launcher's UI (a rewind indicator) and for tests. */
size_t diatom_rewind_depth(void);

/* Frees the ring and stops the worker thread. For process exit. */
void diatom_rewind_shutdown(void);

/* Blocks until every capture handed to the worker is committed. For tests,
 * which capture faster than any frame loop would; never call it from the
 * frame loop. */
void diatom_rewind_flush(void);

#endif
