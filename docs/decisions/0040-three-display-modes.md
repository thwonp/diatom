# 0040. Offer three display modes - integer, aspect, stretch; default stays stretch

- **Status:** Accepted
- **Date:** 2026-10-02
- **Supersedes:** ADR-0014 (the mode set), ADR-0018 (integer-vertical)
- **Superseded by:** -

## Context

- Seven modes existed: integer, integer-vertical, aspect, fill, stretch,
  overscale, native. They were a comparison set (ADR-0014), kept so the
  default could be chosen by looking at a panel. It was: stretch.
- Coverage and crop were computed per console from `scale.c`'s own maths on
  both panels (GKD 1600x1440, Brick 1024x768; tables in the plorpOS seq 33
  handoff, calculator `gkd62-display/modes.py`). What they showed:
  - **fill** gives the same picture as **aspect** wherever the core's aspect
    matches the panel's, and elsewhere crops it: GKD Genesis loses 71
    columns, GKD GBA 62, Brick Game Boy 24 rows.
  - **overscale** crops on both axes nearly everywhere (GKD Arcade 155
    columns + 18 rows) and keeps integer's shape errors (29% wide on CPS2).
  - **native** is 1-11% of the panel on every console - a reference, not a
    way to play.
  - **integer-vertical** matches **aspect** where the vertical factor is
    already whole (GKD NES, PC Engine, PlayStation at exactly 5x) and is
    otherwise a smaller aspect picture (GKD GBA 60% vs 74%).
- The user's verdict on the full list: "I don't think half of those display
  options would be desirable for anyone", and the cut applies to every
  console. The first cut kept integer-vertical; the user then removed it too
  (2026-10-02).

## Options considered

### Option A - keep stretch, aspect, integer, integer-vertical
The first proposal the user chose. Keeps the shape-correct whole-row mode for
the consoles where it differs from aspect. Costs a fourth entry whose
difference from aspect is uneven rows under nearest sampling, which few
players can name.

### Option B - keep stretch, aspect, integer (chosen)
The three answers to "how big should the picture be" that a player can tell
apart: whole pixels, the shape the core asks for, the whole panel. Costs
integer-vertical's whole-row shape-correct picture, and anyone who wanted
uniform pixels with the right shape on SNES or GBA now picks between integer
(right rows, wrong width on non-square-pixel cores) and aspect (right shape,
uneven rows).

### Option C - keep all seven
Nothing breaks. But every cycle of the display hotkey walks four modes the
tables say nobody should pick, and the frontend menu repeats them.

## Decision

`diatom_modes` is integer, aspect, stretch, in that cycle order. The
removed modes' enum values and arithmetic are deleted, not hidden. A
`SETDISPLAY` or `--display` naming a removed mode is refused like any unknown
name (`bad_display`). The default stays `stretch` (ADR-0014): the user kept
it.

## Consequences

- The display hotkey cycles three modes, so any mode is at most two presses
  away.
- A frontend that saved a removed mode must map it itself; plorpOS maps all
  four to aspect when it loads its settings.
- The crop path in the ports' blit (a rect larger than the panel) is now
  reached only by integer showing an oversized source 1:1 (SNES hires).
- Bringing a mode back means restoring its `scale_rect` case from git
  history, not flipping a flag.

## Revisit if

A console is added whose aspect and resolution make aspect and integer both
clearly wrong on a shipped panel (for example a non-square-pixel source whose
whole factor overshoots by more than 20% wide or narrow), or a player asks for
a cropped full-panel mode by name.
