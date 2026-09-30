/* NextUI-derived: PolyForm Noncommercial 1.0.0, NOT this repo's root MIT
 * license - see THIRD-PARTY.md before touching this file. */
/* See rewind.h. Concept ported from NextUI's ma_rewind.c (PolyForm
 * Noncommercial 1.0.0) - see THIRD-PARTY.md. What differs from that port:
 * NextUI's ring is LZ4-compressed and filled by a worker thread; this one is
 * uncompressed and filled synchronously from the frame loop, because Diatom
 * has no measured per-core state size on the actual target hardware to size
 * a compression tradeoff against (open question, carried from the TortOS
 * epic's bead for this feature - see the bd issue). Uncompressed is the
 * simpler, obviously-correct starting point; LZ4 is a depth-for-CPU trade to
 * revisit once real sizes are measured on a Brick, not before.
 *
 * No SDL, no port header - deliberately, so a test can link this with a fake
 * diatom_core and no display, the same reason db.c and save.c stay clean of
 * their own device dependencies. */
#include <stdlib.h>
#include <string.h>

#include "rewind.h"

/* Total ring memory, all slots combined, as a first cut pending real
 * per-core measurement on the Brick. 8 MiB against a ~1GB device (see
 * diatom's own README) is comfortably affordable even doubled by a size
 * that grows mid-session (see the per-capture re-check below). It is NOT
 * several minutes of rewind, as this comment once claimed: an mGBA state is
 * 463 KB, so 8 MiB is ~18 slots, ~4.5 s (plorpos-gkd.24).
 *
 * A port may override the budget, the capture cadence (rewind.h) and the
 * depth cap from the Makefile. The GKD does, after measuring ~1.5 GB free
 * with a game running (plorpos-gkd.38); the Brick keeps these defaults until
 * it is measured the same way. Slots are allocated as the ring fills, not up
 * front, so a large budget costs nothing until the session is long enough
 * to use it. */
#ifndef DIATOM_REWIND_BUDGET_BYTES
#define DIATOM_REWIND_BUDGET_BYTES (8u * 1024 * 1024)
#endif

/* DIATOM_REWIND_CAPTURE_EVERY (rewind.h): a snapshot taken every frame would
 * spend more CPU copying state than the core spends producing it, for no
 * benefit - rewinding one frame at a time is not something a human aims
 * for. 15 frames is a quarter second at 60fps and an eighth of a second at
 * NextUI's own slowest supported core rate; matches the granularity NextUI
 * exposes as configurable (FE_OPT_REWIND_*) rather than picking a different
 * number with no measurement to prefer it over this one. */

#define DIATOM_REWIND_MIN_DEPTH 8
#ifndef DIATOM_REWIND_MAX_DEPTH
#define DIATOM_REWIND_MAX_DEPTH 600
#endif

typedef struct {
	void   *buf;
	size_t  cap;   /* allocated bytes */
	size_t  len;   /* bytes actually holding a valid snapshot */
} rewind_slot;

static rewind_slot *g_slots;
static size_t       g_cap_slots;   /* ring depth, fixed for the session */
static size_t       g_head;        /* next slot capture() will write */
static size_t       g_count;       /* valid entries currently held */
static unsigned     g_tick;        /* frames since the last capture */
static unsigned     g_every = DIATOM_REWIND_CAPTURE_EVERY;   /* 0 = off */

static void free_slots(void)
{
	size_t i;
	if (!g_slots) return;
	for (i = 0; i < g_cap_slots; i++) free(g_slots[i].buf);
	free(g_slots);
	g_slots = NULL;
	g_cap_slots = g_head = g_count = 0;
	g_tick = 0;
}

void diatom_rewind_reset(diatom_core *c)
{
	size_t n, depth;

	free_slots();
	g_every = DIATOM_REWIND_CAPTURE_EVERY;
	if (!c || !c->serialize_size) return;

	n = c->serialize_size();
	if (!n) return;   /* this core has no savestate support - no ring */

	depth = DIATOM_REWIND_BUDGET_BYTES / n;
	if (depth < DIATOM_REWIND_MIN_DEPTH) depth = DIATOM_REWIND_MIN_DEPTH;
	if (depth > DIATOM_REWIND_MAX_DEPTH) depth = DIATOM_REWIND_MAX_DEPTH;

	g_slots = calloc(depth, sizeof *g_slots);
	if (!g_slots) return;   /* rewind unavailable; everything else still runs */
	g_cap_slots = depth;
}

void diatom_rewind_shutdown(void) { free_slots(); }

void diatom_rewind_capture(diatom_core *c)
{
	rewind_slot *s;
	size_t n;

	if (!g_slots || !g_every || !c || !c->serialize_size || !c->serialize) return;
	if (++g_tick < g_every) return;
	g_tick = 0;

	/* Re-checked every capture, never cached - save.c's own comment measured
	 * a core's serialize_size() changing mid-session (mGBA, Golden Sun), and
	 * a slot sized for the old number would corrupt whichever slot came
	 * after it in memory if this call trusted a stale size. */
	n = c->serialize_size();
	if (!n) return;

	s = &g_slots[g_head];
	if (s->cap < n) {
		void *grown = realloc(s->buf, n);
		if (!grown) return;   /* keep the old, smaller snapshot rather than lose the slot */
		s->buf = grown;
		s->cap = n;
	}
	if (!c->serialize(s->buf, n)) return;
	s->len = n;

	g_head = (g_head + 1) % g_cap_slots;
	if (g_count < g_cap_slots) g_count++;
}

bool diatom_rewind_step_back(diatom_core *c)
{
	rewind_slot *s;

	if (!g_slots || !g_count || !c || !c->unserialize) return false;

	g_head = (g_head + g_cap_slots - 1) % g_cap_slots;
	s = &g_slots[g_head];
	g_count--;

	/* A grown-then-failed-to-grow slot (see capture, above) can hold a
	 * shorter snapshot than the core's CURRENT serialize_size expects to
	 * consume. Restoring it anyway would hand the core a stale state, which
	 * is a wrong picture but not a wrong-sized buffer - unserialize reads
	 * `s->len` bytes, exactly what was captured, never more. */
	return s->len && c->unserialize(s->buf, s->len);
}

size_t diatom_rewind_depth(void) { return g_count; }

bool diatom_rewind_set_every(int every)
{
	size_t i;

	if (every < 0 || every > DIATOM_REWIND_MAX_EVERY) return false;
	g_every = (unsigned)every;
	g_tick = 0;
	if (!g_every && g_slots) {
		/* Off means off: no history to step into, and none of the memory
		 * a full ring holds. The slot table stays, so turning it back on
		 * mid-session just starts filling again. */
		for (i = 0; i < g_cap_slots; i++) {
			free(g_slots[i].buf);
			g_slots[i].buf = NULL;
			g_slots[i].cap = g_slots[i].len = 0;
		}
		g_head = g_count = 0;
	}
	return true;
}

unsigned diatom_rewind_every(void) { return g_every; }
