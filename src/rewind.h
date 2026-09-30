/* NextUI-derived: PolyForm Noncommercial 1.0.0, NOT this repo's root MIT
 * license - see THIRD-PARTY.md before touching this file. */
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
 * 3x - plorpos-gkd.24). */
#ifndef DIATOM_REWIND_CAPTURE_EVERY
#define DIATOM_REWIND_CAPTURE_EVERY 15
#endif

/* Called once per RUN, before the frame loop starts. Frees any previous
 * session's buffers and sizes a fresh ring against the core's CURRENT
 * serialize_size() and a fixed memory budget. A core that reports 0 (no
 * savestate support) gets a ring of depth 0 - rewind is simply unavailable
 * that session, the same way it would be with no snapshots yet captured. */
void diatom_rewind_reset(diatom_core *c);

/* Call periodically during ordinary forward play (see DIATOM_REWIND_CAPTURE_
 * EVERY in rewind.c) - NOT while rewind is active, or the timeline it is
 * walking backward through would be overwritten under it. Re-checks
 * serialize_size() on every call rather than trusting the size the ring was
 * built with, because a core's state size can grow mid-session (measured on
 * mGBA, see save.c) - a slot too small for the new size is grown in place. */
void diatom_rewind_capture(diatom_core *c);

/* Steps one entry backward: restores the most recently captured state via
 * unserialize and consumes it from the ring. False means the ring is
 * exhausted - the caller should hold the current frame rather than call
 * this again, matching "rewind stops at the oldest recorded moment" rather
 * than wrapping around to imagined future frames. */
bool diatom_rewind_step_back(diatom_core *c);

/* How many steps remain before diatom_rewind_step_back would return false.
 * For the launcher's UI (a rewind indicator) and for tests. */
size_t diatom_rewind_depth(void);

/* Frees the ring. Same effect as diatom_rewind_reset with a NULL core, kept
 * as its own name for callers that are shutting down rather than starting a
 * new session. */
void diatom_rewind_shutdown(void);

#endif
