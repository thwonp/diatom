# 0038. Let the player choose the hotkey modifier, default MENU, menu on release

- **Status:** Accepted
- **Date:** 2026-09-30
- **Supersedes:** - (amends ADR-0035's "SELECT is the modifier")
- **Superseded by:** -

## Context

- ADR-0035 made SELECT the one frontend modifier, and it is suppressed from
  the core whenever it is held. A core sees SELECT only on the first frame of
  a press, because it reads input inside `retro_run` before the mask is set.
  So a game can never hold SELECT, and one that samples it slowly can miss it.
  Most games use SELECT for something (user, 2026-09-30).
- MENU is never a game input on any device. It paused the game on its press
  edge.
- NextUI uses MENU as the shortcut modifier: a tap opens its menu, and MENU+button
  is a shortcut.
- The GKD has a free Home key (ADR-0037). The GKD and the Brick Pro have a stick
  click that no shipped core uses.

## Options considered

### Option A - every candidate key is a modifier at once
Nothing to configure. But every one of them is taken from the game for good,
SELECT included, which is the problem this ADR exists to fix.

### Option B - the player chooses one, per device, default MENU
One setting, global rather than per system so the muscle memory is the same in
every game. Choices: MENU, SELECT, L3 (stick click, where there is one) and, on
the GKD, Home. MENU costs the game nothing. The price is that a MENU tap has to
open the menu on release rather than on press, so a hotkey chord can be told
apart from a tap.

## Decision

Option B. `SETHOTKEYS` carries an optional `modifier=` (`menu`, `select`, `l3`;
`home` on the GKD). `HOTKEYS` reports it. It is reset to MENU on every RUN, like
the bindings. A line with a bad modifier or bad bindings is refused whole.

When the modifier is MENU, the pause (or standalone stop) fires on MENU's
release, and only if no other button was pressed while MENU was down. With any
other modifier, MENU acts on its press edge as before.

`DIATOM_BTN_L3` is new. Like MENU, it is never forwarded to a core and is
unmappable. The Brick port reports it only on the Pro, because the same SDL index is the
plain Brick's front brightness key.

## Consequences

- With MENU (the default), SELECT reaches games in full, held or tapped.
- The in-game menu opens on release, not press. That's imperceptible on a tap,
  but a held MENU opens nothing until it is let go.
- Choosing SELECT brings back the one-frame SELECT tap. That is now the
  player's trade, not the default.
- Pressing an unbound button while MENU is held cancels the menu without
  firing anything. That's the price of telling a chord from a tap.

## Revisit if

- Players report the menu feeling late (release vs press), or opening when
  they meant a chord.
- A shipped core wants L3 as a game input. It would then need forwarding
  when L3 is not the modifier.
