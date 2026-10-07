# 0046. PicoDrive is the Sega core, and 32X is in

- **Status:** Accepted
- **Date:** 2026-10-07
- **Supersedes:** the 2026-08-25 Sega core choice (spike
  `2026-08-25-sega-core-comparison.md`) and the register's "32X is out"
- **Superseded by:** -

## Context

- On 2026-08-25 `genesis_plus_gx` won the Sega block over PicoDrive on one
  finding: PicoDrive "reports a fixed 320x240 for every system". With 32X out
  of scope, PicoDrive was not carried at all.
- That finding was the load-time report. Re-measured 2026-10-07 on
  PicoDrive 2.05-1890c29, every system starts at 320x240 and settles within a
  frame or three to its own geometry: Game Gear 160x144, Master System
  256x192, Genesis 320x224. Diatom follows a settled geometry
  ([ADR-0021](0021-settle-the-rect-before-locking.md)), so the picture was
  never going to be the double-scaled one the spike predicted. The spike read
  the first line; `tools/corefacts.sh` already warns that the first line is
  the boot mode.
- The user wants 32X, and accepts that save states made under
  `genesis_plus_gx` will not load.
- The register's 32X reasons were cost (a sixth core for ~40 titles) and an
  unmeasured question (dual SH-2 on Cortex-A53). With PicoDrive replacing
  `genesis_plus_gx` there is no extra core, and the speed is now measured.

## Decision

- `picodrive` covers Genesis, 32X, Sega CD, Master System and Game Gear.
  `genesis_plus_gx` is no longer pinned.
- Hosts choose the aspect: PicoDrive's default `PAR` is square pixels; its
  `CRT` is within 3% of what `genesis_plus_gx` reported for all three systems.
  TortOS sets `picodrive_aspect=CRT`. `docs/reference/core-facts.md` records
  the core's default, as it does for every core.

## Evidence

- Speed, 30 s each through TortOS, **0 resyncs on every run**: Brick and GKD
  at 60.0 fps, RG SP (H700) at its panel's 59.57 - Virtua Racing Deluxe,
  Knuckles' Chaotix, Metal Head and Doom (50, PAL dumps), Lunar: Eternal Blue
  (Sega CD), Phantasy Star IV, Game Gear and Master System titles. Popful Mail
  and Kolibri held too, standalone on the Brick.
- Memory: mapped and idle, PicoDrive dirties 96 kB (GKD smaps); a core's
  idle cost tracks its writable segment, 6.5 MB for `genesis_plus_gx`.
- Binary: 1.9 MB against 12.6 MB; needs GLIBC 2.33 at most, so the buildbot
  build loads on the Brick as fetched.

## Consequences

- **States break.** ADR-0016's header refuses a `genesis_plus_gx` state with a
  warning and the game starts fresh, Continue included.
- **Sega CD saves move.** `genesis_plus_gx` kept one backup RAM per region
  (`scd_U.brm` in the save directory); PicoDrive keeps the same raw 8 KB
  image per disc as its `.srm` (RAM cart off, its default). A host carries
  them over by copying the old file to a disc that has no `.srm` yet - TortOS
  does, and the copy was verified byte-identical after a PicoDrive run.
- PicoDrive reports 60.0000 / 50.0000 fps rather than 59.9227; content runs
  0.13% fast, below anything audible or visible.
- PicoDrive's first av-info after a game switch on a resident core carries
  the previous game's size, so a log can show a frame larger than `max`.
  Diatom sizes its frame buffer per frame, never from `max`, so this is
  cosmetic.
- License: MAME-style non-commercial, the same restriction class as the core
  it replaces ([ADR-0023](0023-core-licensing.md)).
