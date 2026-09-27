# Third-party notices

Diatom's own code is intended to be permissively licensed (see ADR-0002 for why
that matters - escaping GPL inheritance is a founding goal of the project).

## `src/rewind.c`

The fast-forward and rewind concept (`src/rewind.c`, plus the `SPEED`/
`SETSPEED`/`REWIND`/`SETREWIND` protocol verbs in `src/proto.c` and
`src/diatom.h`) is **ported from NextUI** (`ma_runframe.c`'s
`setFastForward`/`limitFF`, `ma_rewind.c`'s savestate ring buffer) rather than
implemented clean-room. NextUI is licensed **PolyForm Noncommercial 1.0.0**.

This is safe here and would NOT be safe in Diatom's own upstream: this fork
exists as a permanently-diverged, personal, noncommercial fork consumed by a
sibling fork of TortOS (`github.com/thwonp/TortOS`, see that repo's own
`THIRD-PARTY-LICENSES.md`), with no upstreaming intent to
`github.com/ericreinsmidt/diatom`. `src/rewind.c` is therefore
**noncommercial-only**, the same restriction the sibling TortOS fork already
carries for the same reason and, on that fork, for two libretro cores besides.
Every other file in this repo, including the rest of `src/proto.c` and
`src/diatom.h`, remains under the root `LICENSE` (MIT), unchanged - the new
protocol verbs are a straightforward extension of ADR-0020's existing state
plane and carry nothing NextUI-derived in their own text.

See [ADR-0034](docs/decisions/0034-fast-forward-and-rewind.md) for what was
ported, what had to be adapted for this frontend's different architecture
(the launcher/Diatom process split NextUI has no equivalent of), and what
remains genuinely unverified pending real hardware.

## `src/libretro.h`

The libretro API header, vendored so the build is self-contained.

- **Copyright (C) 2010-2024 The RetroArch team**
- **MIT-style permissive license**, and the header states that its license
  statement *"only applies to this libretro API header (libretro.h)"*.

That scoping is deliberate on libretro's part and is what makes the whole
arrangement work: any-license frontends may host any-license cores, because the
interface between them carries no copyleft.

Full text is at the top of the file.

## `vendor/rcheevos/`

RetroAchievements' implementation of their achievement condition language,
vendored under ADR-0025 so the build stays self-contained.

- **Copyright (c) 2018 RetroAchievements.org**
- **MIT licensed**, full text in `vendor/rcheevos/LICENSE`.

MIT is the whole reason this is possible rather than a problem: it is diatom's
own license, so ADR-0002's founding goal - escaping GPL inheritance - is
untouched. The networking halves (`rc_client`, `rapi`, `rurl`) are deliberately
not vendored; `vendor/rcheevos/README.md` lists every omission and why.

## Cores

**Diatom ships no cores and has no core list.** It loads whatever shared library
it is handed at runtime. Cores keep their own licenses - commonly GPL - and are
the responsibility of whoever distributes them.

`test/stubcore.c` is diatom's own code: a libretro core that is not an emulator,
so the frontend can be exercised with no third-party binary present.
