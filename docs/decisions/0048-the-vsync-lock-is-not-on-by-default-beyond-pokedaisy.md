# 0048. The vsync lock is not on by default for cores other than PokeDaisy

- **Status:** Accepted
- **Date:** 2026-11-04
- **Supersedes:** -
- **Superseded by:** -

## Context

The Brick's vsync lock (present waits for the page to latch, the frame loop runs at the panel's rate) came out of the
PokeDaisy frame-pacing work and is on by default for PokeDaisy only. Bead plorpos-rk0.14 asked whether it should also be
the default for other cores, and whether the GKD, RG Rotate and H700 gain from one. Full data:
`~/projects/misc/coding-team/plorpos/rk014/REPORT.md` (outside the repo, by the owner's rule).

**Measured 2026-11-04, 20 s runs, one discarded warm-up then two each, all units on USB (CPU frequency and utilisation
stand in for battery):**

1. **Panel rates are not what the modes say.** Brick 60.23 Hz (latch gaps). H700 59.57 Hz (already panel-paced). GKD
   **59.111 Hz** and Rotate **60.278 Hz** by counting `DRM_IOCTL_WAIT_VBLANK` (300 vblanks), confirmed by the swap cadence
   of a locked swap on every core. The GKD's DRM mode says 60.000 and is wrong by 1.5 %.
2. **Beat = |core rate - panel rate|.** On the Brick it predicted the measured counts: mGBA 0.50 Hz (10 predicted per
   20 s, 11 and 13 measured), PicoDrive 0.23 Hz (5), Snes9x and FCEUmm 0.13 Hz (2.6, measured 2-3), Beetle PCE 0.40 Hz (9).
3. **On the Brick the lock removes those repeats** (mGBA 11-13 to 0-1, PicoDrive 5 to 1-3, FCEUmm 3 to 0-1, PCE 9 to 1,
   Snes9x 2.5 to 0) with 0 audio drops, but it costs the CPU: the `performance` governor pin runs mGBA at 2000 MHz against
   600 MHz unlocked. A minimum-frequency floor of 816 MHz keeps the gain for mGBA, PicoDrive, FCEUmm and PCE (CPU about
   820-940 MHz); Snes9x did not hold the lock at an 816 or 1200 MHz floor (11-15 repeats) and needs the full pin.
4. **The owner could not tell the difference** between lock on and off in a hands-on A/B on those cores.
5. **H700:** 0 repeated, 0 superseded, 0 long on GBA, SNES and DS with its own panel pacing; a lock would add nothing.
6. **GKD and Rotate** (a prototype: `SDL_GL_SetSwapInterval(1)` while locked): the glass cadence goes clean, but the game
   then runs at the panel's rate (GKD about 1 % slow, Rotate 0.5-0.9 % fast), and audio drops tens of thousands of
   frames per 20 s on DSperate (both devices) and thousands on PCSX-ReARMed (GKD). GBA and SNES were clean.
7. PCSX-ReARMed and DSperate gain nothing worth having on the Brick: audio drops under the lock (PCSX), core slower
   than the panel (DSperate: 11.5 to 7 repeats, CPU +21 %).

## Options considered

### Option A - lock by default for the cores that gain, at a CPU floor
Implemented and measured on a branch (a per-core table keyed on `library_name`, `scaling_min_freq` floor). Removes 3-13
repeated frames per 20 s on four cores at a CPU cost of roughly +200 MHz mean. Rejected: the benefit was not perceptible
to the owner, and it adds a per-core table, a second CPU strategy and a game speed change (+0.4-0.9 %, 7-15 cents) to
maintain.

### Option B - lock by default everywhere, governor pinned
Rejected on the data: 3x the clock for light cores, audio drops on PCSX, no gain for DSperate.

### Option C - leave the lock as it was: on for PokeDaisy, opt-in elsewhere
Chosen. The PokeDaisy case is different in kind: its scrolling hitch measured about 88 repeated frames per 45 s in
EXPAND, an order of magnitude more than the cores above.

## Decision

The lock stays on by default for PokeDaisy only. For every other core it is opt-in: a `diatom.vsync` file (Brick) forces
it on, `diatom.novsync` forces it off. The GKD and Rotate keep their deliberate vsync-off; no lock is ported to them and
none to the H700. The glass counters stay (`diatom_port_glass_stats`, one `diatom: glass:` line in the exit summary on the
Brick and H700), because they are the only direct measure of what the panel showed.

## Consequences

- No per-core default table, no CPU floor, no GKD swap-interval lock; they exist only in git history (commits
  `434a4bd`, `953e509`, `a01bbe1` and their reverts on the local branch `feat/vsync-lock`) if the question returns.
- The `Vsync OFF, deliberately` comment in `port/gkd.c` rests on a premise that is now measured: no core matches the
  GKD's 59.111 Hz panel, so every core there drops 37-59 frames a minute, and a locked swap makes the game about 1 %
  slow. The choice is between those two, and the owner has not asked for the second.
- `diatom.loopprof` (what the PokeDaisy `realrun.sh` greps) never existed in a branch; use the glass line instead.

## Revisit when

A user reports judder on a specific core and device; a core's rate changes enough to widen the beat; the GKD or Rotate
audio-under-lock problem (plorpos-rk0.15) is fixed and someone wants the lock there; or battery matters enough to measure
off USB (this study could not).
