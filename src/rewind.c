/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* NextUI-derived: PolyForm Noncommercial 1.0.0, NOT this repo's root MIT
 * license - see NOTICE and THIRD-PARTY.md before touching this file. */
/* See rewind.h. Ported from NextUI's ma_rewind.c (PolyForm Noncommercial
 * 1.0.0) - see THIRD-PARTY.md. Like NextUI, each snapshot is XORed against
 * the one before it and LZ4-compressed on a worker thread into a byte ring
 * of variable-length entries (plorpos-gkd.59). Measured on the GKD from real
 * autosaves, capture every 5 frames: PlayStation 2.0x plain LZ4 -> 53x with
 * the XOR delta, every other system 21-123x, against an uncompressed ring
 * holding ~1 s of PlayStation and ~2 s of PICO-8.
 *
 * What differs from NextUI, all for one rule - the frame loop never waits
 * on the worker and never compresses (the player's bar: no hitch, not even
 * a millisecond):
 *  - a capture with no free buffer is dropped, not compressed in the frame
 *    loop; the next delta is taken against the last committed state, so a
 *    drop costs one entry of granularity, never the chain;
 *  - starting a rewind discards queued and in-flight work (a generation
 *    bump) instead of waiting for the worker to drain;
 *  - no keyframes: the first capture becomes the reference only, every
 *    entry is a delta, and decoding walks backward from the newest state,
 *    so dropping the oldest entry never breaks what remains;
 *  - serialize_size() is re-read every capture (mGBA changes it mid-
 *    session, see save.c) and a change starts the ring over.
 *
 * Entry k holds LZ4(state_k XOR state_k-1); g_ref is state_newest. A step
 * back decodes the newest entry against the state it was taken after and
 * restores state_k-1.
 *
 * No SDL, no port header - deliberately, so a test can link this with a fake
 * diatom_core and no display (test/rewind_test.c). */
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lz4.h"
#include "rewind.h"

/* Total compressed history, all entries combined. The ring is allocated
 * once at this size; pages become real memory only as entries reach them.
 * A port may override the budget, the capture cadence (rewind.h) and the
 * entry cap from the Makefile; the GKD does (plorpos-gkd.38). */
#ifndef DIATOM_REWIND_BUDGET_BYTES
#define DIATOM_REWIND_BUDGET_BYTES (8u * 1024 * 1024)
#endif

/* DIATOM_REWIND_CAPTURE_EVERY (rewind.h): a snapshot every frame would spend
 * more CPU serializing than the core spends producing it, for no benefit -
 * rewinding one frame at a time is not something a human aims for. */

/* The most entries held, however small they compress. Rewind plays one
 * entry per displayed frame, so this is also the longest hold: 600 = 10 s. */
#ifndef DIATOM_REWIND_MAX_DEPTH
#define DIATOM_REWIND_MAX_DEPTH 600
#endif

/* Capture buffers: one being compressed, one waiting. The worker takes a
 * few ms per entry (PlayStation: 6 ms average, 18 ms worst) against a
 * capture every 5-15 frames, so a third would only ever sit idle. */
#define POOL 2

enum { SLOT_FREE, SLOT_FILLING, SLOT_QUEUED, SLOT_BUSY };

typedef struct {
	uint8_t *buf;
	unsigned gen;
	int      state;
} slot;

typedef struct { size_t off, len; } entry;

/* ---- main thread only ---- */
static unsigned g_every = DIATOM_REWIND_CAPTURE_EVERY;   /* 0 = off */
static unsigned g_tick;           /* frames since the last capture */
static bool     g_in_rewind;      /* step_back ran since the last capture */
static bool     g_popped;         /* ...and restored something: g_dec is live */
static uint8_t *g_dbuf;           /* decompressed delta */
static uint8_t *g_dec;            /* the state the last step restored */

/* ---- g_qmx: the queue, slot ownership, and what the worker reads ---- */
static pthread_mutex_t g_qmx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_qcv = PTHREAD_COND_INITIALIZER;     /* work, or stop */
static pthread_cond_t  g_idlecv = PTHREAD_COND_INITIALIZER;  /* a slot came back */
static pthread_t       g_worker;
static bool            g_worker_up, g_stop;
static slot            g_slots[POOL];
static int             g_q[POOL], g_qh, g_qn;
static size_t          g_size;    /* state size every buffer is built for; 0 = none */
static size_t          g_bufcap;  /* bytes each per-state buffer can hold */
static uint8_t        *g_delta, *g_scratch;   /* worker's own */
static int             g_bound;   /* LZ4_compressBound(g_size) */

/* ---- g_rmx: the ring, the reference, the generation ---- */
static pthread_mutex_t g_rmx = PTHREAD_MUTEX_INITIALIZER;
static unsigned g_gen;            /* bumped to discard work in flight */
static uint8_t *g_ref;            /* state_newest; written by the worker only */
static bool     g_have_ref;
static uint8_t *g_ring;
static entry    g_ent[DIATOM_REWIND_MAX_DEPTH];
static size_t   g_ent_tail, g_count;   /* oldest entry, entries held */
static size_t   g_whead;          /* byte offset the next entry tries first */

/* What forward play paid this session, for diatom_rewind_stats. */
static unsigned long g_ser_n, g_drops;
static uint64_t      g_ser_us, g_ser_max_us;

static uint64_t mono_us(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
}

/* XOR a word at a time - byte-wise was 5.7 ms per PlayStation rewind step
 * in the bench, all of it in the frame loop. memcpy keeps it alignment-safe
 * and compiles to plain loads. out may alias a. */
static void xor_into(uint8_t *out, const uint8_t *a, const uint8_t *b, size_t n)
{
	size_t i = 0;
	for (; i + 8 <= n; i += 8) {
		uint64_t x, y;
		memcpy(&x, a + i, 8);
		memcpy(&y, b + i, 8);
		x ^= y;
		memcpy(out + i, &x, 8);
	}
	for (; i < n; i++) out[i] = a[i] ^ b[i];
}

/* ---- the ring (g_rmx held) ---- */

static entry *newest(void) { return &g_ent[(g_ent_tail + g_count - 1) % DIATOM_REWIND_MAX_DEPTH]; }

static void drop_oldest(void)
{
	g_ent_tail = (g_ent_tail + 1) % DIATOM_REWIND_MAX_DEPTH;
	if (!--g_count) g_whead = 0;
}

static void ring_clear(void) { g_ent_tail = g_count = g_whead = 0; }

/* Does any held entry overlap [off, off + n)? */
static bool overlaps(size_t off, size_t n)
{
	size_t i;
	for (i = 0; i < g_count; i++) {
		const entry *e = &g_ent[(g_ent_tail + i) % DIATOM_REWIND_MAX_DEPTH];
		if (e->off < off + n && off < e->off + e->len) return true;
	}
	return false;
}

/* Entries sit end to end, wrapping to offset 0 when the next one would run
 * past the end. History can only lose its oldest end, so the oldest entry
 * goes until nothing still held lies where this one is written. */
static bool ring_put(const uint8_t *src, size_t n)
{
	size_t off = g_whead;

	if (!g_ring || n > DIATOM_REWIND_BUDGET_BYTES) return false;
	if (g_count == DIATOM_REWIND_MAX_DEPTH) drop_oldest();
	if (off + n > DIATOM_REWIND_BUDGET_BYTES) off = 0;
	while (g_count && overlaps(off, n)) drop_oldest();

	memcpy(g_ring + off, src, n);
	g_ent[(g_ent_tail + g_count) % DIATOM_REWIND_MAX_DEPTH] = (entry){ off, n };
	g_count++;
	g_whead = off + n;
	return true;
}

/* ---- the worker ---- */

/* test/rewind_test.c stalls the worker here to hold a capture in flight
 * across the start of a rewind, which timing alone almost never does. */
#ifdef DIATOM_REWIND_TEST_HOOK
void DIATOM_REWIND_TEST_HOOK(void);
#endif

static void *worker(void *arg)
{
	(void)arg;
	pthread_mutex_lock(&g_qmx);
	for (;;) {
		int i, len = 0;
		unsigned gen;
		size_t n;
		uint8_t *buf, *ref;
		bool have;

		while (!g_stop && !g_qn) pthread_cond_wait(&g_qcv, &g_qmx);
		if (g_stop) break;
		i = g_q[g_qh];
		g_qh = (g_qh + 1) % POOL;
		g_qn--;
		g_slots[i].state = SLOT_BUSY;
		gen = g_slots[i].gen;
		buf = g_slots[i].buf;
		n = g_size;
		pthread_mutex_unlock(&g_qmx);

		/* g_ref and g_have_ref change only here, or on the main thread
		 * while no slot is busy - safe to read without g_rmx. */
		ref = g_ref;
		have = g_have_ref;
		if (have) {
			xor_into(g_delta, buf, ref, n);
			len = LZ4_compress_default((const char *)g_delta, (char *)g_scratch, (int)n, g_bound);
		}

#ifdef DIATOM_REWIND_TEST_HOOK
		DIATOM_REWIND_TEST_HOOK();
#endif
		pthread_mutex_lock(&g_rmx);
		if (gen == g_gen) {
			/* No reference yet: this state becomes it. A delta that could
			 * not be stored breaks the chain behind it, so the ring starts
			 * over from here rather than decode garbage later. */
			if (have && !(len > 0 && ring_put(g_scratch, (size_t)len))) ring_clear();
			/* The capture buffer becomes the reference; the old reference
			 * becomes the free capture buffer. No copy. */
			g_slots[i].buf = g_ref;
			g_ref = buf;
			g_have_ref = true;
		}
		pthread_mutex_unlock(&g_rmx);

		pthread_mutex_lock(&g_qmx);
		g_slots[i].state = SLOT_FREE;
		pthread_cond_broadcast(&g_idlecv);
	}
	pthread_mutex_unlock(&g_qmx);
	return NULL;
}

/* g_qmx held. Nothing queued and nothing being compressed. */
static bool idle_locked(void)
{
	int i;
	for (i = 0; i < POOL; i++)
		if (g_slots[i].state == SLOT_QUEUED || g_slots[i].state == SLOT_BUSY) return false;
	return true;
}

/* g_qmx held. Queued captures go back to the pool unprocessed. */
static void discard_queue_locked(void)
{
	while (g_qn) {
		g_slots[g_q[g_qh]].state = SLOT_FREE;
		g_qh = (g_qh + 1) % POOL;
		g_qn--;
	}
}

/* Main thread, g_qmx held, worker idle. Every per-state buffer, sized for
 * n; n = 0 frees them. History built for another size is meaningless, so
 * it goes too. */
static bool size_buffers_locked(size_t n)
{
	int i;
	uint8_t **bufs[] = { &g_slots[0].buf, &g_slots[1].buf, &g_ref, &g_delta, &g_dbuf, &g_dec };

	pthread_mutex_lock(&g_rmx);
	ring_clear();
	g_have_ref = false;
	g_gen++;
	pthread_mutex_unlock(&g_rmx);
	g_popped = false;

	if (n > g_bufcap || !n) {
		for (i = 0; i < (int)(sizeof bufs / sizeof *bufs); i++) { free(*bufs[i]); *bufs[i] = NULL; }
		free(g_scratch);
		g_scratch = NULL;
		g_bufcap = 0;
		g_size = 0;
		if (!n) return true;
		for (i = 0; i < (int)(sizeof bufs / sizeof *bufs); i++)
			if (!(*bufs[i] = malloc(n))) return size_buffers_locked(0), false;
		if (!(g_scratch = malloc((size_t)LZ4_compressBound((int)n))))
			return size_buffers_locked(0), false;
		g_bufcap = n;
	}
	g_size = n;
	g_bound = LZ4_compressBound((int)n);
	return true;
}

/* Main thread. Blocks until the worker is idle - only for calls made
 * outside the frame loop (a new RUN, the speed menu, shutdown). */
static void wait_idle_locked(void)
{
	discard_queue_locked();
	while (!idle_locked()) pthread_cond_wait(&g_idlecv, &g_qmx);
}

static void free_all(void)
{
	pthread_mutex_lock(&g_qmx);
	wait_idle_locked();
	size_buffers_locked(0);
	pthread_mutex_unlock(&g_qmx);
	pthread_mutex_lock(&g_rmx);
	free(g_ring);
	g_ring = NULL;
	pthread_mutex_unlock(&g_rmx);
	g_tick = 0;
	g_in_rewind = g_popped = false;
}

void diatom_rewind_reset(diatom_core *c)
{
	(void)c;   /* sized lazily, against each capture's own serialize_size() */
	free_all();
	g_every = DIATOM_REWIND_CAPTURE_EVERY;
	g_ser_n = g_drops = 0;
	g_ser_us = g_ser_max_us = 0;
}

void diatom_rewind_shutdown(void)
{
	free_all();
	if (g_worker_up) {
		pthread_mutex_lock(&g_qmx);
		g_stop = true;
		pthread_cond_signal(&g_qcv);
		pthread_mutex_unlock(&g_qmx);
		pthread_join(g_worker, NULL);
		g_worker_up = g_stop = false;
	}
}

void diatom_rewind_flush(void)
{
	pthread_mutex_lock(&g_qmx);
	while (!idle_locked()) pthread_cond_wait(&g_idlecv, &g_qmx);
	pthread_mutex_unlock(&g_qmx);
}

void diatom_rewind_capture(diatom_core *c)
{
	int i, s = -1;
	size_t n;

	if (!g_every || !c || !c->serialize_size || !c->serialize) return;
	if (++g_tick < g_every) return;
	g_tick = 0;

	n = c->serialize_size();
	if (!n || n > (size_t)LZ4_MAX_INPUT_SIZE) return;

	pthread_mutex_lock(&g_qmx);
	/* Every path below that needs the worker idle checks rather than waits:
	 * if it is still busy, this capture is dropped and the next one tries
	 * again. */
	if (g_in_rewind) {
		if (!idle_locked()) goto drop;
		/* Rewind ended. The next delta must be against the state the
		 * player rewound to, or the remaining history decodes wrong. */
		if (g_popped) {
			uint8_t *t = g_ref;
			g_ref = g_dec;
			g_dec = t;
		}
		g_in_rewind = g_popped = false;
	}
	if (n != g_size) {
		if (!idle_locked()) goto drop;
		if (!size_buffers_locked(n)) goto drop;
	}
	if (!g_ring) {
		pthread_mutex_lock(&g_rmx);
		g_ring = malloc(DIATOM_REWIND_BUDGET_BYTES);
		pthread_mutex_unlock(&g_rmx);
		if (!g_ring) goto drop;
	}
	if (!g_worker_up) {
		if (pthread_create(&g_worker, NULL, worker, NULL)) goto drop;
		g_worker_up = true;
	}
	for (i = 0; i < POOL; i++)
		if (g_slots[i].state == SLOT_FREE) { s = i; break; }
	if (s < 0) goto drop;   /* worker behind: skip, never wait */
	g_slots[s].state = SLOT_FILLING;
	pthread_mutex_unlock(&g_qmx);

	/* The only cost forward play pays: the core's own serialize. */
	{
		uint64_t t0 = mono_us(), dt;
		bool ok = c->serialize(g_slots[s].buf, n);

		dt = mono_us() - t0;
		g_ser_n++;
		g_ser_us += dt;
		if (dt > g_ser_max_us) g_ser_max_us = dt;
		if (!ok) {
			pthread_mutex_lock(&g_qmx);
			g_slots[s].state = SLOT_FREE;
			goto drop;
		}
	}

	pthread_mutex_lock(&g_qmx);
	g_slots[s].gen = g_gen;
	g_slots[s].state = SLOT_QUEUED;
	g_q[(g_qh + g_qn) % POOL] = s;
	g_qn++;
	pthread_cond_signal(&g_qcv);
	pthread_mutex_unlock(&g_qmx);
	return;
drop:
	g_drops++;
	pthread_mutex_unlock(&g_qmx);
}

bool diatom_rewind_step_back(diatom_core *c)
{
	entry *e;
	bool ok;

	if (!c || !c->unserialize || !g_size) return false;

	if (!g_in_rewind) {
		/* Whatever the worker has not committed yet is dropped, not waited
		 * for: the newest ~1/12 s of history, against a hitch. */
		g_in_rewind = true;
		g_popped = false;
		pthread_mutex_lock(&g_qmx);
		discard_queue_locked();
		pthread_mutex_unlock(&g_qmx);
		pthread_mutex_lock(&g_rmx);
		g_gen++;
		pthread_mutex_unlock(&g_rmx);
	}

	/* The worker commits nothing after the bump above, so this lock waits
	 * at most for a commit already under way - a memcpy of one entry. */
	pthread_mutex_lock(&g_rmx);
	if (!g_count || !g_have_ref) {
		pthread_mutex_unlock(&g_rmx);
		return false;
	}
	e = newest();
	ok = LZ4_decompress_safe((const char *)g_ring + e->off, (char *)g_dbuf,
	                         (int)e->len, (int)g_size) == (int)g_size;
	if (ok) xor_into(g_dec, g_dbuf, g_popped ? g_dec : g_ref, g_size);
	if (!--g_count) g_whead = 0;
	else g_whead = e->off;
	if (!ok) ring_clear();   /* the chain behind a bad entry is unusable */
	pthread_mutex_unlock(&g_rmx);

	if (!ok) return false;
	g_popped = true;
	return c->unserialize(g_dec, g_size);
}

void diatom_rewind_stats(diatom_rewind_stat *st)
{
	size_t i;

	pthread_mutex_lock(&g_rmx);
	st->entries = g_count;
	st->bytes = 0;
	for (i = 0; i < g_count; i++)
		st->bytes += g_ent[(g_ent_tail + i) % DIATOM_REWIND_MAX_DEPTH].len;
	pthread_mutex_unlock(&g_rmx);
	st->budget = DIATOM_REWIND_BUDGET_BYTES;
	st->every = g_every;
	st->captures = g_ser_n;
	st->dropped = g_drops;
	st->serialize_avg_us = g_ser_n ? (unsigned)(g_ser_us / g_ser_n) : 0;
	st->serialize_max_us = (unsigned)g_ser_max_us;
}

size_t diatom_rewind_depth(void)
{
	size_t n;
	pthread_mutex_lock(&g_rmx);
	n = g_count;
	pthread_mutex_unlock(&g_rmx);
	return n;
}

bool diatom_rewind_set_every(int every)
{
	if (every < 0 || every > DIATOM_REWIND_MAX_EVERY) return false;
	g_every = (unsigned)every;
	g_tick = 0;
	/* Off means off: no history to step into, and none of the memory a
	 * full ring holds. Turning it back on starts filling again. */
	if (!g_every) free_all();
	return true;
}

unsigned diatom_rewind_every(void) { return g_every; }
