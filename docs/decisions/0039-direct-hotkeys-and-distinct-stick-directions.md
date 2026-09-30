# 0039. Direct hotkey triggers, and stick directions distinct from the d-pad

- **Status:** Accepted
- **Date:** 2026-09-30
- **Supersedes:** - (extends ADR-0035's binding table and ADR-0038's modifier)
- **Superseded by:** -

## Context

- Every hotkey needed the modifier held (ADR-0035, ADR-0038). Many systems
  leave buttons unused (a GBA game has no X, Y, L2 or R2), and on those a
  one-button hotkey is faster than a chord (user, 2026-09-30).
- A modifier chord leaks its button to the core for one frame, because the
  core reads input inside `retro_run` before the mask is set.
- The user wants d-pad and stick directions bindable, and the d-pad must
  never stop working for a system (user, 2026-09-30).
- Both ports with a stick (Brick Pro, GKD) folded it onto the d-pad bits, so
  nothing above the port could tell Stick Left from d-pad Left.

## Options considered

### Stick: the port sets both the d-pad bit and a stick bit
No change for the core. But a bound Stick Left could only be hidden from the
game by hiding the d-pad's Left too, because the port had already set it.

### Stick: the port sets only stick bits, the host folds them onto the d-pad
The fold happens after the suppress mask, in the one place core input is
read. A hidden stick direction is hidden alone, and the d-pad is untouched.

### Wire: bare `x:ff` means direct
The natural reading, but every stored spec was a modifier binding. Bare
`r2:ff` would silently become a direct R2 that hides R2 from the game.

### Wire: bare `x:ff` stays the modifier layer, `d.x:ff` is direct
Stored specs keep their meaning. Nothing needs purging.

## Decision

- New `DIATOM_BTN_SUP/SDOWN/SLEFT/SRIGHT` (`sup sdown sleft sright`). Ports
  report the stick only on these. `env.c`'s `core_input()` folds them onto
  UP/DOWN/LEFT/RIGHT after `g_suppress`, for both `cb_input_state` and
  turbo. They are never forwarded as themselves, and SETMAP refuses them.
- A trigger is an input on a layer. `x:ff` needs the modifier held; `d.x:ff`
  is direct. Direct inputs are L1 R1 L2 R2 A B X Y. The modifier layer also
  takes `up down left right sup sdown sleft sright`.
- Each action has at most one trigger. Each (input, layer) holds at most one
  action, so X and `d.X` may drive different actions.
- `hotkey_chord()` always suppresses direct-bound buttons, so they never leak.
  While the modifier is held only modifier bindings fire, and the modifier
  plus modifier-bound inputs are suppressed. Otherwise only direct bindings
  fire. Switching layers releases a held FF or rewind.

## Consequences

- A direct-bound button is gone from that system's games entirely. That is
  the player's choice, per system, and the reason directions cannot be direct.
- Holding the modifier and pressing a button that has only a direct binding
  does nothing: the modifier layer wins.
- To a game, the stick still behaves exactly like the d-pad.

## Revisit if

- A shipped core wants analog stick input.
- A device's stick should be a second pad rather than the d-pad.
