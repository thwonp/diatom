# Third-party notices

This fork is MIT (root `LICENSE`) - Eric Reinsmidt's code and the fork's own -
except the NextUI-derived code below, which is PolyForm Noncommercial 1.0.0
(`LICENSES/`). `NOTICE` lists it file by file and region by region, and each
source file's SPDX line says which it is. See
[ADR-0042](docs/decisions/0042-license-per-file-mit-default.md), which
superseded the whole-fork PolyForm license of ADR-0036 on 2026-10-03 and
restored ADR-0023's permissive premise for the MIT parts.

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
`src/diatom.h`, carries no NextUI provenance - the new
protocol verbs are a straightforward extension of ADR-0020's existing state
plane and carry nothing NextUI-derived in their own text. The two fenced
fast-forward regions of `src/main.c` (its state and its deadline) are the
part of `setFastForward`/`limitFF` that was ported, and carry the same terms.

See [ADR-0034](docs/decisions/0034-fast-forward-and-rewind.md) for what was
ported, what had to be adapted for this frontend's different architecture
(the launcher/Diatom process split NextUI has no equivalent of), and what
remains genuinely unverified pending real hardware.

## `src/hotkeys.c`, `src/hotkeys.h`

The hotkey submenu concept - a SELECT-held chord binding fast-forward,
rewind, quicksave and quickload to a player-chosen button - is **ported
from NextUI** (`ma_frontend_opts.c`'s `OptionShortcuts_*`), same terms as
`src/rewind.c` above: PolyForm Noncommercial 1.0.0, noncommercial-only,
consumed only by the sibling personal fork of TortOS, never upstreamed.
The `hotkey_chord()` dispatch function in `src/main.c` that actually checks
these bindings each frame is the same NextUI-derived concept and carries the
same restriction, even though it lives outside `hotkeys.c` itself (it was
modeled on the older `display_chord()`, which plorpos-gkd.22 folded into it
- see [ADR-0035](docs/decisions/0035-hotkey-submenu-lives-on-select.md)).

The `HOTKEYS`/`SETHOTKEYS` protocol verbs (`src/proto.c`, `src/diatom.h`)
are, like `SPEED`/`SETSPEED`/`REWIND`/`SETREWIND` before them, a plain
extension of ADR-0020's state plane and carry nothing NextUI-derived in
their own text - no NextUI provenance.

See [ADR-0035](docs/decisions/0035-hotkey-submenu-lives-on-select.md) for
what was ported, why NextUI's own MENU-held convention specifically was NOT
(fact 3 there), and the verification ceiling this was written against.

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

MIT is the whole reason this is possible rather than a problem: it is
compatible with diatom's own license (MIT, upstream and in this fork), and
brings no GPL code in. The networking halves (`rc_client`, `rapi`, `rurl`) are deliberately
not vendored; `vendor/rcheevos/README.md` lists every omission and why.

## `vendor/lz4/`

The LZ4 block compressor, vendored for the rewind ring (`src/rewind.c`,
plorpos-gkd.59) because the GKD's ROCKNIX image ships no `liblz4.so`.

- **Copyright (c) 2011-2020, Yann Collet**
- **BSD 2-Clause licensed**, full text in `vendor/lz4/LICENSE`.

Permissive, like rcheevos: compatible with this fork's license and brings no
copyleft in. Only `lib/lz4.c` and `lib/lz4.h` of v1.10.0 are carried, unmodified;
`vendor/lz4/README.md` says why the frame and HC formats are not.

## `vendor/stb/`

`stb_image_write.h` v1.16, the screenshot hotkey's PNG encoder (`src/shot.c`,
plorpos-gkd.86.2), vendored because neither device image ships a libpng.

- **Sean Barrett** and contributors
- **Public domain (Unlicense) or MIT**, the reader's choice; taken as MIT. Both
  texts are at the end of the header.

Unmodified. `vendor/stb/stb_image_write.c` is this repo's own one-line wrapper
that compiles the implementation; `vendor/stb/README.md` says why.

## Cores

**Diatom ships no cores and has no core list.** It loads whatever shared library
it is handed at runtime. Cores keep their own licenses - commonly GPL - and are
the responsibility of whoever distributes them.

`test/stubcore.c` is diatom's own code: a libretro core that is not an emulator,
so the frontend can be exercised with no third-party binary present.
