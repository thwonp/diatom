# 0042. License per file: MIT by default, PolyForm Noncommercial only where NextUI's code is

- **Status:** Accepted
- **Date:** 2026-10-03 (accepted 2026-10-03, plorpos-4g4)
- **Supersedes:** [ADR-0036](0036-relicense-polyform-noncommercial.md), in full
- **Superseded by:** -

**Not legal advice**, same as ADR-0023 and ADR-0036.

## Context

ADR-0036 relicensed the whole fork under PolyForm Noncommercial 1.0.0
(2026-09-28), choosing its option B over option A, "stay MIT, carve out each
NextUI-derived file". The reason was convenience: more NextUI-derived work was
planned, and one license for everything meant no per-file bookkeeping.

That turned out to cost more than it saved. Almost none of the fork is
NextUI's: in diatom the ported code is `src/rewind.{c,h}`, `src/hotkeys.{c,h}`
and three regions of `src/main.c` (fast-forward state, the fast-forward
deadline, `hotkey_chord()`); in the sibling TortOS fork it is the sleep and
suspend code and `src/chdread.{c,h}`. Everything else is Eric Reinsmidt's MIT
code or the fork's own, and 0036 put all of it under noncommercial terms for
the sake of those few files. The owner of the fork's own code wants it MIT
(2026-10-02/03: "I want all my code that I own the licence for to be MIT").

Checked 2026-10-03: `git shortlog -sne` in both repositories lists two authors
only, Eric Reinsmidt and Thwonp. There is no third contributor whose code would
need their own consent.

## Options considered

### Option A - per file (0036's option A)
MIT is the default. NextUI-derived code keeps NextUI's PolyForm Noncommercial
1.0.0, marked file by file with SPDX identifiers; a file that is only partly
derived carries `MIT AND PolyForm-Noncommercial-1.0.0` and comment fences
around the derived regions, so nothing has to move away from upstream's layout.
Costs bookkeeping: every new port has to be marked where it lands.

### Option B - stay whole-fork PolyForm (0036)
No bookkeeping, and the cost 0036 accepted: the fork's own code, which has
nothing of NextUI's in it, cannot be used commercially, cannot go back
upstream, and is not GPL-compatible.

### Option A, going forward only
MIT from now on; what was published under PolyForm stays PolyForm. Rejected
by the owner as unnecessary - the code is theirs to license, and older
versions can be offered under MIT just as well.

## Decision

**Option A, for every version.** All code Eric Reinsmidt wrote is MIT, as he
released it. All code Thwonp owns is MIT - in this version and in every earlier
one, including what was published while 0036 was in force. Only NextUI-derived
code is PolyForm Noncommercial 1.0.0, listed in `NOTICE` and marked in each
file:

- `src/rewind.c`, `src/rewind.h`, `src/hotkeys.c`, `src/hotkeys.h` - whole
  files, `SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0`.
- `src/main.c` - `MIT AND PolyForm-Noncommercial-1.0.0`, with
  `BEGIN/END PolyForm-Noncommercial-1.0.0` fences around the fast-forward and
  rewind state, the hotkey chords and `hotkey_chord()`, and the fast-forward
  deadline.

The SPEED, REWIND, REWINDSPEED and HOTKEYS protocol verbs in `src/diatom.h` and
`src/proto.c` are MIT: they are an extension of ADR-0020's state plane and
carry nothing of NextUI's in their own text, which is what THIRD-PARTY.md has
said since they were written. Their comments now say where the NC code they
drive lives.

`LICENSE` is the MIT License; PolyForm's text moves to
`LICENSES/PolyForm-Noncommercial-1.0.0.txt`. A build bundles both, so a built
diatom or a release containing it is noncommercial as a whole; the MIT parts
on their own are not.

## Consequences

- ADR-0023's premise is back for the MIT parts: MIT is GPL-compatible, and
  diatom shipping no cores keeps the GPL cores separate works either way. Its
  three rules never stopped standing. 0023's status, left at Proposed by an
  oversight, is now Accepted.
- Every new port from NextUI must land with its SPDX line or fence, and an
  entry in `NOTICE` - the bookkeeping 0036 avoided. A file with no SPDX line
  is a mistake, not a default.
- The NC regions stay where upstream would expect them; nothing was moved to
  isolate them.
- The sibling TortOS fork made the same change the same day (its `NOTICE`).
