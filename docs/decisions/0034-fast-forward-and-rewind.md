# 0034. Fast-forward and rewind, ported from NextUI, budgeted rather than measured

- **Status:** Accepted
- **Date:** 2026-09-27 (accepted 2026-09-27)
- **Supersedes:** -
- **Superseded by:** -

Extends [ADR-0020](0020-shared-state-plane.md), which is unchanged: this adds
two states to the plane it defines and bends none of its rules.

## Context

TortOS (the launcher this repo serves) has neither feature today, and a
personal, permanently-diverged, noncommercial fork of it is adding both -
authorized to port NextUI's implementation directly rather than reimplement
clean-room, because that fork will never upstream and NextUI's PolyForm
Noncommercial 1.0.0 license permits exactly that use. See TortOS's
`THIRD-PARTY-LICENSES.md` and this repo's `THIRD-PARTY.md`.

What NextUI actually does, read from `ma_runframe.c` and `ma_rewind.c`:

1. **Fast-forward is a frontend-side frame-timing throttle**, not libretro's
   `fast_forward_ratio` environment call. `setFastForward()`/`limitFF()`
   compute a shorter per-frame delay against `fps*(max_ff_speed+1)` and enforce
   it with `SDL_Delay`. The core is asked for exactly as many frames as it
   always was; it is simply not made to wait between them.
2. **Rewind is a real ring buffer of savestate snapshots**, filled by a worker
   thread during ordinary play and stepped backward one entry per frame while
   held - not a repeated reload of one save slot.

What is architecturally different here, which is the entire reason this is a
new ADR and not a copy of NextUI's code:

3. **This frontend already pages absolutely, on a schedule kept in floating
   point** (`run_session_inner`'s `next_us`/`frame_us`), not with a flat
   `SDL_Delay` per frame. Fast-forward here has to change what that schedule
   targets, not bolt a second timer beside it.
4. **`serialize_size()` is not stable across a session.** `save.c`'s own
   comment measured mGBA's report shrink from 528448 to 462912 bytes within
   600 frames of one game, and states outright: "a cached size that later
   grows makes saving fail silently." Any rewind buffer has to re-check this
   before every capture, not size itself once from a value taken at load.
5. **No per-core state size has been measured on the actual target (the
   Brick, ~1GB RAM per this project's own README).** The register's own
   `[LB]` tag on this question is about exactly that gap. NextUI's answer
   (LZ4-compressed, configurable depth via `FE_OPT_REWIND_*`) is a tradeoff
   this repo cannot currently verify is the right one here, because it has no
   measurement to weigh it against.
6. **The environment this was implemented in has no SDL2 installed** and
   cannot install it (no privilege to). `make check-seam`, `check-register`,
   `check-cheevos`\*, and a new SDL-free harness against a fake `diatom_core`
   all ran; `check-proto`, `check-stateplane`, and anything needing `$(BIN)`
   or the stub-core conformance suite (`conform-check`) could not, because
   building the desktop port needs a real SDL2 to link against. \*check-cheevos
   fails to link on this host over a pre-existing missing `-lm`, unrelated to
   this change - not investigated further, out of scope here.

## Options considered

### Option A - libretro's `fast_forward_ratio` / rewind via `RETRO_ENVIRONMENT`

Ask the core to run faster itself, or lean on any core-side rewind support.

Rejected: almost no core in `CORES.md` implements either, NextUI didn't use
them for exactly that reason, and it would make the feature core-dependent
rather than uniform across all eleven systems this project ships.

### Option B - port NextUI's ring buffer verbatim, LZ4 and all

Byte-for-byte the sourcing permission allows.

Rejected for now, not permanently. NextUI's depth is tuned against measured
RAM budgets on its own supported devices; this repo has none for the Brick.
Porting a tuned-but-unverified number in is worse than an honestly-labeled
placeholder, because it looks measured and is not. LZ4 is also a CPU-for-depth
trade with nothing to weigh it against here yet - see fact 5.

### Option C - a fixed, conservative memory budget, uncompressed, sized against the *current* `serialize_size()` at ring creation, re-verified every capture

8 MiB total per session, split into a slot depth by whatever one state
currently costs, clamped to [8, 600] slots. No compression. Every slot grows
in place if a later capture needs more room than it was given (fact 4).

Chosen. It is honestly a placeholder - the `Revisit if` below says exactly
what retires it - but it is a *safe* placeholder: 8 MiB against a ~1GB device
is affordable even doubled by drift, it degrades to a shorter rewind rather
than a crash on a core with unusually large state, and it costs nothing to
change later because nothing else depends on the number.

## Decision

**Option C for rewind; a divisor on the existing pacing schedule for
fast-forward. Both join ADR-0020's state plane.**

```
launcher → SETSPEED speed=4    run at 4x, still asking the core for one frame at a time
launcher → SPEED               query
Diatom   → SPEED speed=4       the reply; never sent unsolicited

launcher → SETREWIND on=1      step the ring backward, one entry per displayed frame
launcher → REWIND              query
Diatom   → REWIND on=1         the reply; never sent unsolicited
```

ADR-0020's ownership table gains two rows:

| State | Defined by | Written by | Diatom emits unsolicited |
|---|---|---|---|
| **Speed** | **launcher** | **launcher** | **no** |
| **Rewind** | **launcher** | **launcher** | **no** |

- **Fast-forward changes the pacing target, not the frame count.**
  `run_session_inner`'s per-iteration `next_us += frame_us` became
  `next_us += frame_us / g_ff_speed`, recomputed every iteration because
  speed can change mid-session, unlike `frame_us` itself (fact 3). The resync
  threshold (`> frame_us * 4.0`) scales with it for the same reason.
- **Fast-forward quiets audio for its duration** through the existing
  `SETQUIET` switch (ADR-0032) - a core still emits one frame of audio per
  `retro_run()` regardless of the sleep between calls, so unthrottled sound
  would play sped-up and garbled. Restored on the way out only if
  fast-forward is what quieted it, so a launcher's own independent
  `SETQUIET` survives a speed change untouched.
- **The ring is per-session** (`src/rewind.c`), reset on every `RUN` against
  the newly-loaded core's current `serialize_size()`, and freed at session
  end. A core reporting 0 (no savestate support) gets a depth-0 ring;
  rewind is silently unavailable, discovered the same way `SAVE`/`LOAD`
  already discover it.
- **Every capture re-reads `serialize_size()`** rather than trusting the
  size the ring was built with (fact 4), growing a slot in place if it no
  longer fits. A slot that fails to grow keeps its last, smaller snapshot
  rather than losing the ring entry.
- **Stepping back restores state, then runs the core once** to render the
  frame that state corresponds to - libretro's `retro_unserialize` restores
  memory, it does not itself produce a video frame. This costs one frame of
  forward drift per rewind step, judged imperceptible against the capture
  granularity (a quarter second at 60fps) it is stepping through.
- **An exhausted ring falls through to ordinary forward play** rather than
  freezing. No special case was needed: capture is simply skipped whenever
  `g_rewind_active` is true, so history stops growing exactly where it stops
  shrinking, and running out of history just resumes play from wherever
  rewinding stopped.
- **Verification ceiling, stated plainly rather than assumed away:** the
  protocol parsing (both new verbs, plus a regression check that an existing
  verb still parses) was tested against the real `proto.c` over a real Unix
  socket. The ring's capture/step-back/growth/wraparound/exhaustion logic was
  tested against a fake `diatom_core` with controlled `serialize_size` -
  including reproducing fact 4's exact scenario (size growing between two
  captures) and confirming neither slot corrupts. The pacing math and the
  quiet-during-FF interaction were reviewed, not executed: no core, no ROM,
  and no way to build `$(BIN)` were available to run `conform-check` or play
  a real session. Nothing here should be read as "verified on hardware."

## Consequences

**Easier.** Fast-forward and rewind exist as launcher-drivable capabilities
without touching the port or the core-loading path; the hotkey submenu
(sibling feature) has something to bind buttons to.

**Harder.** Two more states each side's parser and the launcher's UI have to
carry. And the rewind depth is a guess: it will be too shallow, too deep, or
wasting RAM relative to what a measured budget would choose, until fact 5 is
resolved.

**Forecloses (for now):** LZ4 compression, and any depth NextUI's own tuning
would suggest - both need the measurement this ADR does not have.

**Not yet exercised:** the interaction between fast-forward and cheevos frame
evaluation (`diatom_cheevos_frame()` runs once per loop iteration regardless
of speed, so an achievement condition gets checked at the same per-frame rate
during FF, just packed into less wall time - reasoned through, not run against
a real set).

## Revisit if

- Someone runs `tools/corefacts.sh`-style measurement of real
  `serialize_size()` values across the pinned cores on an actual Brick. That
  retires the `[LB]` tag with a number instead of a budget.
- The 8 MiB budget is ever observed to matter against the device's real
  memory pressure (resident cores, Muse, the launcher itself) - `conform` /
  `conform-device` are the existing tools for that once `$(BIN)` can be built
  here or on a machine with SDL2 installed.
- A core is found whose `serialize_size()` is large enough that
  `DIATOM_REWIND_MIN_DEPTH` (8) gives a rewind window judged too short to be
  useful - that is a real product question, not a memory one, and wants a
  configurable depth (NextUI's `FE_OPT_REWIND_*`) rather than a bigger fixed
  budget.

## Revisited 2026-09-30 (plorpos-gkd.24)

The first `Revisit if` fired, on the GKD rather than the Brick. mGBA's state
is 463 KB, so the 8 MiB ring held ~18 slots: ~4.5 s of play, played back in
~0.3 s (one slot per displayed frame is 15x). Measured on the GKD with a game
and Muse over Bluetooth running: ~1.5 GB still available, one core busy
(plorpos-gkd.38 holds the table).

- The budget, the capture cadence and the depth cap are now build-time
  defaults a port may override (`DIATOM_REWIND_BUDGET_BYTES`,
  `DIATOM_REWIND_CAPTURE_EVERY`, `DIATOM_REWIND_MAX_DEPTH`). The Brick keeps
  8 MiB / 15 / 600 until it is measured the same way (plorpos-gkd.38).
- The GKD sets 256 MiB, every 5 frames, cap 1800: rewind plays at 5x (the
  cadence IS the speed - one slot per displayed frame) and holds ~47 s of
  GBA, ~10 s of holding the hotkey, smooth on the device. Chosen over keeping
  every 15 frames (~141 s, but 15x) and every 3 frames (3x, ~28 s) - the
  player tried 5x and kept it.
- After each session Diatom calls `malloc_trim(0)`, after `EXIT`, so a
  resident Diatom does not keep a large ring's worth of freed heap between
  games.
- Compressed since plorpos-gkd.59, as NextUI does: each snapshot is XORed
  against the one before and LZ4-compressed (vendored, `vendor/lz4/`) on a
  worker thread into a byte ring of variable-length entries; the budget is
  now compressed bytes. Measured on the GKD from real autosaves, capture
  every 5: PlayStation 2.0x -> 53x, every other system 21-123x, so all of
  them reach the 1800-entry cap (~30 s of hold) where PlayStation had ~1 s
  and PICO-8 ~2 s. Unlike NextUI the frame loop never compresses and never
  waits: a capture the worker is not ready for is dropped, and starting a
  rewind discards work in flight instead of draining it. The frame loop
  still pays the core's own serialize every capture.
- The speed is also the player's (plorpos-gkd.40): `SETREWINDSPEED every=N`
  sets the cadence for the session, 0..60, 0 = off (nothing captured, the
  ring emptied and freed, the rewind hotkey a no-op); `REWINDSPEED` asks,
  and both answer `REWINDSPEED every=N`. Every `RUN` resets it to the build's
  default and the launcher re-sends the player's choice after `RUN`, as it
  does `SETHOTKEYS`. The budget stays fixed, so a slower speed is a shorter
  history - accepted by the player. No `proto=` bump, as with this ADR's
  other verbs: an older Diatom ignores it and keeps its default speed.
