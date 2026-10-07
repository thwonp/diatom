# 0045. Turbo Assign: Diatom reports the request, the launcher maps it

- **Status:** Accepted
- **Date:** 2026-10-07
- **Supersedes:** - (extends ADR-0028's turbo and ADR-0039's hotkey actions)
- **Superseded by:** -

## Context

- Turbo was a per-system default in the launcher's map (`turbo.<tag>`,
  `x:a~3,y:b~3`), and on Game Boy Advance mGBA's own Turbo L/R sat on L2/R2.
  Both took buttons away from the hotkeys. The user wants turbo chosen in the
  game instead, per button, and those defaults gone (plorpos-tkh).
- The design is the user's: a hotkey, **Turbo Assign**; press it, then a
  button, and that button's turbo flips; press it again first to cancel; hold
  it 2 s to clear every turbo button. Assignments last for the game session -
  through the launcher's menu and Muse - and end when the game quits.
- ADR-0028 already pulses a button (`src:dst~N`, first frame of a press always
  on, per source before the OR) and rejected a second table answering "what
  does X do". ADR-0020: the launcher owns configuration.

## Decision

- `turbo` is a sixth hotkey action (`HK_TURBO`), bound on either layer like
  the others. `src/turboassign.c` (MIT, tested by `check-turboassign`) turns
  each frame into one of: armed, cancelled, toggle a button, clear.
- Assignable: A, B, X, Y, L1, R1, L2, R2. Never a direction, START, SELECT or
  a Diatom-own key. The choosing press is hidden from the hotkeys and the game
  until it is let go.
- Diatom tells the launcher and nothing else:

  ```
  Diatom -> TURBO arm=1        hotkey pressed: waiting for a button
  Diatom -> TURBO arm=0        hotkey again: cancelled
  Diatom -> TURBO btn=<name>   flip this button's turbo
  Diatom -> TURBO clear=1      held 2 s: clear all
  ```

- The launcher keeps the session's turbo buttons, answers with a whole
  `SETMAP` (ADR-0020, ADR-0028), and shows the notice. RUN resets the map
  to identity as it always has, so a quit game's turbo cannot reach the next.
  An armed hotkey is dropped at RUN and at RESUME.

## Consequences

- One map, one pulse engine, still: Diatom keeps no turbo state of its own
  beyond "armed".
- A launcher that predates this ignores `TURBO` (ADR-0009): the hotkey arms
  and nothing more happens. No version bump.
- A turbo button that maps to nothing in a game (X on NES) pulses nothing.
