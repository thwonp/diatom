/* SPDX-License-Identifier: MIT */
/* SDL's video init leaves the fatal signals as it found them (TortOS
 * plorpos-6ei.10).
 *
 * SDL2's evdev keyboard (SDL_evdev_kbd.c, started by the video driver) takes
 * SIGSEGV, SIGBUS and the other fatal signals for an "emergency cleanup" that
 * puts the console keyboard back, saving what it replaced. Its quit puts the
 * saved handler back only if the current one is still its own. On a port that
 * brings video up and down per game - the Brick's and H700's GL window, up
 * only while a shader is set - that meant:
 *
 *   GBA, shader    SDL saves our crash handler, installs its own
 *   DS             DSperate's JIT installs its fastmem handler, once per
 *                  process; SDL's quit sees it is not SDL's and leaves it
 *   GBA, shader    SDL saves the JIT's handler, installs its own over it
 *   DS             the JIT does not install again, so its first fastmem fault
 *                  - routine, pages map on demand - lands in SDL's handler,
 *                  which re-raises into a chain of handlers until the stack
 *                  is gone
 *
 * Measured on the plain Brick 2026-10-29: every second DS launch after a
 * shader GBA game died inside its first retro_run, no report. Diatom owns this
 * process's signals; the console keyboard SDL would restore is one Diatom
 * never touches. */
#ifndef DIATOM_PORT_SIGNALS_H
#define DIATOM_PORT_SIGNALS_H

#include <signal.h>

/* SDL_evdev_kbd.c's fatal_signals. */
static const int diatom_fatal_signals[] = {
	SIGHUP, SIGQUIT, SIGILL, SIGABRT, SIGFPE, SIGSEGV, SIGPIPE, SIGBUS, SIGSYS,
};
#define DIATOM_FATAL_SIGNALS \
	(int)(sizeof diatom_fatal_signals / sizeof diatom_fatal_signals[0])

static inline void diatom_signals_save(struct sigaction *saved)
{
	int i;

	for (i = 0; i < DIATOM_FATAL_SIGNALS; i++)
		sigaction(diatom_fatal_signals[i], NULL, &saved[i]);
}

static inline void diatom_signals_restore(const struct sigaction *saved)
{
	int i;

	for (i = 0; i < DIATOM_FATAL_SIGNALS; i++)
		sigaction(diatom_fatal_signals[i], &saved[i], NULL);
}

#endif
