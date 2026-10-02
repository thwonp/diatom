/* src/rewind.c against a fake core: every restored state is checked byte for
 * byte, so a wrong delta reference, a ring entry overwritten while still
 * held, or a stale capture slipping through all fail here instead of as a
 * subtly wrong picture on a device. Built twice by `make check-rewind` -
 * ASan+UBSan and TSan - because the worker thread is half of what is being
 * tested.
 *
 * Built with a small ring (see the Makefile): 64 KiB, 40 entries, against
 * 4 KiB states, so the budget and the entry cap are both reachable. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "rewind.h"

static int failures;

#define CHECK(cond, ...)                                                      \
	do {                                                                      \
		if (!(cond)) {                                                        \
			printf("  FAIL: ");                                               \
			printf(__VA_ARGS__);                                              \
			printf("\n        at %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
			failures++;                                                       \
		}                                                                     \
	} while (0)

/* ---- a core whose whole state is a function of (frame, variant) ---------- */

#define SZ 4096
static size_t g_sz = SZ;
static int    g_frame, g_var;
/* 0: a small moving window, deltas compress to almost nothing. 1: every
 * byte changes each frame, deltas do not compress. 2: a frame-dependent
 * number of bytes change, so entries vary in size - what makes the ring
 * wrap with older entries still lying past the write head. */
static int    g_noisy;
static bool   g_bad_restore;  /* unserialize got bytes no capture could have produced */

static void make_state(uint8_t *out, size_t n, int frame, int var)
{
	uint32_t x = (uint32_t)frame * 2654435761u + (uint32_t)var * 40503u + 1;
	size_t i;

	uint32_t h = (uint32_t)frame * 2654435761u;
	size_t loud;

	h ^= h >> 15; h *= 0x2c1b3c6du; h ^= h >> 12;
	loud = g_noisy == 1 ? n : g_noisy == 2 ? h % n : 0;

	for (i = 0; i < n; i++) {
		if (i < loud) {
			x ^= x << 13; x ^= x >> 17; x ^= x << 5;
			out[i] = (uint8_t)x;
		} else {
			out[i] = (uint8_t)(i * 7 + var);
		}
	}
	if (!g_noisy)   /* like a game's RAM: a little changes per frame */
		for (i = 0; i < 16; i++) out[(frame * 16 + i) % n] = (uint8_t)(frame + i);
	memcpy(out, &frame, sizeof frame);
	memcpy(out + sizeof frame, &var, sizeof var);
}

static size_t fake_size(void) { return g_sz; }

static bool fake_serialize(void *buf, size_t n)
{
	if (n != g_sz) return false;
	make_state(buf, n, g_frame, g_var);
	return true;
}

static bool fake_unserialize(const void *buf, size_t n)
{
	uint8_t *want = malloc(n);
	int frame, var;
	bool same;

	memcpy(&frame, buf, sizeof frame);
	memcpy(&var, (const uint8_t *)buf + sizeof frame, sizeof var);
	make_state(want, n, frame, var);
	same = n == g_sz && !memcmp(buf, want, n);
	free(want);
	if (!same) { g_bad_restore = true; return false; }
	g_frame = frame;
	g_var = var;
	return true;
}

static diatom_core g_core = {
	.serialize_size = fake_size,
	.serialize      = fake_serialize,
	.unserialize    = fake_unserialize,
};

/* ---- helpers -------------------------------------------------------------- */

static void start(void)
{
	diatom_rewind_reset(&g_core);
	diatom_rewind_set_every(1);
	g_sz = SZ;
	g_var = 0;
	g_noisy = 0;
	g_bad_restore = false;
}

static void play(int from, int to, int var, bool flush)
{
	g_var = var;
	for (g_frame = from; g_frame <= to; g_frame++) {
		diatom_rewind_capture(&g_core);
		if (flush) diatom_rewind_flush();
	}
	g_frame = to;
}

/* Steps back until the ring is exhausted, checking each restore lands on
 * expect[i] (frame, variant pairs); n < 0 means only "strictly older each
 * time". Returns how many steps succeeded. */
static int rewind_all(const int (*expect)[2], int n)
{
	int steps = 0, last = 1 << 30;

	while (diatom_rewind_step_back(&g_core)) {
		if (n >= 0) {
			CHECK(steps < n, "more steps than expected (%d)", n);
			if (steps < n)
				CHECK(g_frame == expect[steps][0] && g_var == expect[steps][1],
				      "step %d restored frame %d var %d, want %d var %d",
				      steps, g_frame, g_var, expect[steps][0], expect[steps][1]);
		} else {
			CHECK(g_frame < last, "step %d went from %d to %d", steps, last, g_frame);
		}
		last = g_frame;
		steps++;
		if (steps > 10000) break;
	}
	CHECK(!g_bad_restore, "a restore handed the core bytes no capture produced");
	CHECK(diatom_rewind_depth() == 0, "exhausted, yet depth %zu", diatom_rewind_depth());
	return steps;
}

static int seq[256][2];
static const int (*down(int hi, int lo, int var))[2]
{
	int i = 0, f;
	for (f = hi; f >= lo; f--, i++) { seq[i][0] = f; seq[i][1] = var; }
	return (const int (*)[2])seq;
}

/* ---- tests ---------------------------------------------------------------- */

static void test_sequence(void)
{
	puts("every capture, restored newest first");
	start();
	play(1, 30, 0, true);
	/* The first capture is the reference only; 29 deltas behind frame 30. */
	CHECK(diatom_rewind_depth() == 29, "depth %zu", diatom_rewind_depth());
	CHECK(rewind_all(down(29, 1, 0), 29) == 29, "steps");
	CHECK(!diatom_rewind_step_back(&g_core), "exhausted ring must keep saying no");
}

static void test_entry_cap(void)
{
	puts("the entry cap drops the oldest");
	start();
	play(1, 100, 0, true);
	CHECK(diatom_rewind_depth() == 40, "depth %zu", diatom_rewind_depth());
	CHECK(rewind_all(down(99, 60, 0), 40) == 40, "steps");
}

static void test_budget(void)
{
	int n;
	puts("the byte budget drops the oldest");
	start();
	g_noisy = 1;
	play(1, 60, 0, true);
	n = (int)diatom_rewind_depth();
	CHECK(n >= 10 && n <= 16, "64 KiB of ~4 KiB entries held %d", n);
	CHECK(rewind_all(down(59, 60 - n, 0), n) == n, "steps");
}

static void test_varied_sizes(void)
{
	int n;
	puts("entries of varying size wrap without losing what is held");
	start();
	g_noisy = 2;
	play(1, 300, 0, true);
	n = (int)diatom_rewind_depth();
	CHECK(n >= 10, "depth %d", n);
	/* Exactly contiguous: a decode failure would cut it short. */
	CHECK(rewind_all(down(299, 300 - n, 0), n) == n, "steps");
}

static void test_drops(void)
{
	int n;
	puts("captures the worker cannot keep up with are dropped, not waited for");
	start();
	play(1, 200, 0, false);
	diatom_rewind_flush();
	n = (int)diatom_rewind_depth();
	CHECK(n >= 1, "depth %d", n);
	CHECK(rewind_all(NULL, -1) == n, "steps");
}

static void test_size_change(void)
{
	puts("a serialize_size change starts the history over");
	start();
	play(1, 10, 0, true);
	g_sz = SZ * 2;
	play(11, 20, 0, true);
	CHECK(diatom_rewind_depth() == 9, "depth %zu", diatom_rewind_depth());
	CHECK(rewind_all(down(19, 11, 0), 9) == 9, "steps");
}

static void test_resume_after_rewind(void)
{
	int i;
	puts("history taken after a rewind chains onto what is left");
	start();
	play(1, 20, 0, true);
	for (i = 0; i < 5; i++) CHECK(diatom_rewind_step_back(&g_core), "step %d", i);
	CHECK(g_frame == 15, "rewound to %d", g_frame);
	play(16, 25, 1, true);   /* a different future from frame 15 */
	for (i = 0; i < 9; i++) { seq[i][0] = 24 - i; seq[i][1] = 1; }
	for (i = 9; i < 24; i++) { seq[i][0] = 15 - (i - 9); seq[i][1] = 0; }
	CHECK(rewind_all((const int (*)[2])seq, 24) == 24, "steps");
}

static void test_rewind_mid_flight(void)
{
	puts("rewinding while captures are queued discards them");
	start();
	play(1, 50, 0, false);
	rewind_all(NULL, -1);   /* no flush: whatever was committed, in order */
	play(51, 60, 0, false);
	diatom_rewind_flush();
	rewind_all(NULL, -1);
}

/* The worker stalls before committing for as long as this says. */
static _Atomic int g_stall_ms;
void rewind_test_hook(void) { if (g_stall_ms) usleep(g_stall_ms * 1000); }

static void test_commit_after_rewind_starts(void)
{
	int steps;
	puts("a capture still compressing when rewind starts is never committed");
	start();
	play(1, 20, 0, true);
	g_stall_ms = 100;
	g_frame = 21;
	diatom_rewind_capture(&g_core);   /* frame 21 goes in flight... */
	usleep(20 * 1000);                /* ...and the worker picks it up */
	CHECK(diatom_rewind_step_back(&g_core) && g_frame == 19, "first step to %d", g_frame);
	usleep(200 * 1000);               /* the stalled capture finishes here */
	g_stall_ms = 0;
	steps = rewind_all(down(18, 1, 0), 18);
	CHECK(steps == 18, "steps %d", steps);
}

static void test_off(void)
{
	puts("speed 0 empties the ring and turning it back on refills it");
	start();
	play(1, 10, 0, true);
	diatom_rewind_set_every(0);
	CHECK(diatom_rewind_depth() == 0, "depth %zu", diatom_rewind_depth());
	CHECK(!diatom_rewind_step_back(&g_core), "nothing to step into");
	play(11, 15, 0, true);
	CHECK(diatom_rewind_depth() == 0, "off captures nothing");
	diatom_rewind_set_every(1);
	play(16, 20, 0, true);
	CHECK(diatom_rewind_depth() == 4, "depth %zu", diatom_rewind_depth());
	CHECK(rewind_all(down(19, 16, 0), 4) == 4, "steps");
}

static void test_cadence(void)
{
	puts("every=3 captures every third frame");
	start();
	diatom_rewind_set_every(3);
	play(1, 30, 0, true);   /* captures 3, 6, ... 30 */
	CHECK(diatom_rewind_depth() == 9, "depth %zu", diatom_rewind_depth());
	{
		int i;
		for (i = 0; i < 9; i++) { seq[i][0] = 27 - 3 * i; seq[i][1] = 0; }
		CHECK(rewind_all((const int (*)[2])seq, 9) == 9, "steps");
	}
}

static void test_shutdown_in_flight(void)
{
	puts("shutdown with work in flight, then a fresh session");
	start();
	play(1, 50, 0, false);
	diatom_rewind_shutdown();
	CHECK(diatom_rewind_depth() == 0, "depth %zu", diatom_rewind_depth());
	start();
	play(1, 5, 0, true);
	CHECK(rewind_all(down(4, 1, 0), 4) == 4, "steps");
}

int main(void)
{
	test_sequence();
	test_entry_cap();
	test_budget();
	test_varied_sizes();
	test_drops();
	test_size_change();
	test_resume_after_rewind();
	test_rewind_mid_flight();
	test_commit_after_rewind_starts();
	test_off();
	test_cadence();
	test_shutdown_in_flight();
	diatom_rewind_shutdown();

	if (failures) {
		printf("\n%d check(s) failed\n", failures);
		return 1;
	}
	printf("\nok: every check passed\n");
	return 0;
}
