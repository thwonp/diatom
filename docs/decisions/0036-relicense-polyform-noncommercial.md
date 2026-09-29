# 0036. Relicense this fork under PolyForm Noncommercial 1.0.0

- **Status:** Accepted
- **Date:** 2026-09-28 (accepted 2026-09-28)
- **Supersedes:** [ADR-0023](0023-core-licensing.md), in part (its license premise; its three rules stand)
- **Superseded by:** -

**Not legal advice**, same as ADR-0023.

## Context

This repository is a permanently diverged fork, the emulator of plorpOS
(working name), itself a fork of TortOS. Upstream cannot take NextUI-derived
code for license reasons, and its author welcomed the fork going its own way.

The fork was already not MIT-only. [ADR-0034](0034-fast-forward-and-rewind.md)
and [ADR-0035](0035-hotkey-submenu-lives-on-select.md) ported NextUI code
(PolyForm Noncommercial 1.0.0) into `src/rewind.c`, `src/hotkeys.c` and
`hotkey_chord()`, tracked as per-file carve-outs in `THIRD-PARTY.md`. More
NextUI-derived work is planned, including the GKD 350H Ultra port.

## Options considered

### Option A - stay MIT, carve out each NextUI-derived file
The status quo. It keeps Eric Reinsmidt's permissive framing for most files, but
every port adds a carve-out, the line between the two licenses runs through
single files (`src/main.c`), and a reader can't tell what they may do with a
file without reading `THIRD-PARTY.md`.

### Option B - the whole fork under PolyForm Noncommercial 1.0.0
One license for the fork's code, the same one NextUI uses, so NextUI code can
be used under its terms without bookkeeping. MIT permits sublicensing, provided
Eric Reinsmidt's notice is kept, which `NOTICE` does. Costs: the fork is no
longer open source by the OSI definition, and it loses GPL compatibility (see
Consequences).

## Decision

**Option B.** The root `LICENSE` is PolyForm Noncommercial 1.0.0. `NOTICE`
carries the `Required Notice:` line and Eric Reinsmidt's MIT notice in full.
Every `SPDX-License-Identifier` in the fork's own files reads
`PolyForm-Noncommercial-1.0.0`. Vendored code (`src/libretro.h`,
`vendor/rcheevos`) keeps its own MIT notices. The sibling TortOS fork made the
same change in the same step.

## Consequences

**ADR-0023's settling argument no longer holds.** It concluded that a
GPL core cannot cause a conflict because *"MIT is GPL-compatible"*. PolyForm
Noncommercial is not GPL-compatible. Under the strictest reading (the FSF's:
a dynamically loaded plugin that shares data structures with its host forms one
combined work), a *distributed* bundle of this diatom and a GPL core (`fceumm`,
`mednafen_pce_fast`, `mednafen_ngp`) has no license it can be conveyed under.

What that does and doesn't change:

- **This repository is unaffected.** ADR-0023's rule 1 still holds: diatom
  ships no cores, so distributing this source raises nothing.
- **The exposure is on whoever ships a card image with GPL cores on it**, which
  for plorpOS is the fork's own author, for hobby use. It is not new: it has
  existed since ADR-0034 put noncommercial code into diatom, including in the
  already-published `v1.0.1-thwonp` card.
- ADR-0023's rules 2 and 3 (keep `libretro.h`'s scoped notice; record core
  licenses with the tool that fetches them) stand unchanged.

**Easier.** NextUI code can be used under its terms without carve-outs. One
license per file, and it is stated in the file.

**Harder.** The fork can't be a permissive base for other firmware, which was
the upstream README's pitch. Anyone who wants that should use upstream diatom.

## Revisit if

- A GPL core's copyright holder objects, or card images start being distributed
  beyond hobby sharing. The options then are to replace the GPL cores with
  permissive or noncommercial ones, or to stop putting GPL cores on published
  images (players fetch them).
- The fork's author decides to make it commercial, in which case every
  NextUI-derived part, and this license, stops working.
