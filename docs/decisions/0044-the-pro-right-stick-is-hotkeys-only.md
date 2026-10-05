# 0044. The Brick Pro's right stick and R3 are hotkeys only

- **Status:** Accepted
- **Date:** 2026-10-05
- **Supersedes:** - (extends ADR-0038's modifier list and ADR-0039's triggers)
- **Superseded by:** -

## Context

- The Brick Pro has a second stick and a second stick click. The port read
  neither: the right stick's axes (3/4, measured on the device 2026-09-28)
  and its click (SDL button 10) were dropped.
- No shipped core wants a second stick or a stick click. All are digital, and
  the left stick already folds onto the d-pad (ADR-0039).
- The user wants R3 as a modifier choice and the right stick's directions as
  hotkey triggers (plorpos-pky.17, 2026-10-05).
- ADR-0039 keeps directions off the direct layer, because a direct binding hides
  its input from the game and the d-pad must never stop working.

## Options considered

### Option A - the right stick is a second d-pad, or analog input for the core
That's the libretro-faithful mapping. But no shipped core would read it, and
analog passthrough contradicts ADR-0003. This is cost with no user.

### Option B - the right stick and R3 are Diatom's own, like L3
New bits that are never forwarded and never mappable. Because no game sees
them, a direct binding hides nothing. So the right stick's directions are
allowed on both layers, unlike the left stick's.

## Decision

Option B.
- New bits `DIATOM_BTN_R3` and `DIATOM_BTN_RSUP/RSDOWN/RSLEFT/RSRIGHT`, with
  wires `r3`, `rsup`, `rsdown`, `rsleft` and `rsright`. They come after the
  left stick's bits, so the fold's layout is unchanged. They are folded onto
  nothing, map to no retropad id, and SETMAP refuses them.
- `SETHOTKEYS` accepts `modifier=r3`. It accepts `rsup`-style and `d.rsup`-style
  triggers.
- The Brick port reports R3 on button 10 only on the Pro, because on the plain
  Brick that is a front brightness key. It reads the right stick with the left
  stick's thresholds: half travel to press, a third to let go.

## Consequences

- On the Pro, a player can have up to eight extra one-handed hotkeys without
  giving up a single game input.
- Holding the left stick's thumb on the d-pad while flicking the right stick is
  a two-hand chord, and nothing prevents binding it. That is the player's choice.
- A spec naming the right stick is valid on every device. Only the Pro ever
  reports those bits, and the launcher offers them only there.

## Revisit if

- A shipped core wants a second stick or analog input.
- Another device with two sticks is ported: its port reports the same bits.
