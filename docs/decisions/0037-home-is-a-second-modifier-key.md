# 0037. Let a spare device key stand in for SELECT as the frontend modifier

- **Status:** Accepted
- **Date:** 2026-09-29 (accepted 2026-09-29)
- **Supersedes:** [ADR-0035](0035-hotkey-submenu-lives-on-select.md), in part (its "one modifier key" premise; its rejection of MENU stands)
- **Superseded by:** -

## Context

1. The GKD 350H Ultra has two keys the Brick lacks: **Menu** (`BTN_MODE`, the
   same code as the Brick's MENU, so it is `DIATOM_BTN_MENU`) and **Home**
   (`BTN_TRIGGER_HAPPY1`, 704). Measured by evtest on 2026-09-29.
2. The user wanted in-game hotkeys on a dedicated key rather than SELECT, which
   games also use. Their first ask was Menu (`Menu+L2` = load state).
3. ADR-0035 fact 3 still holds: MENU opens the pause menu instantly on press,
   and making it a hold-modifier would force open-on-release, a felt latency
   regression on a load-bearing control.
4. Home has no existing meaning in the host. The port already uses it,
   port-side, to turn the volume keys into brightness keys; volume keys never
   reach the host, so that use cannot collide with a chord.

## Options considered

### Option A - MENU as the modifier (the user's first ask)

Rejected by ADR-0035 fact 3, unchanged.

### Option B - Home replaces SELECT on the GKD

Hotkeys and display chords would be Home-only on the GKD, SELECT-only on the
Brick. Rejected: the same binding would mean different key presses per device,
and a GKD player loses the SELECT chords the Brick has taught them.

### Option C - Home as a second, independent modifier with its own chords

What ADR-0035 Option C rejected: a second thing to remember and test, for no
capability the first one lacks.

### Option D - Home as an alias of SELECT-as-modifier

A new canonical `DIATOM_BTN_HOTKEY`, reported by a port that has a key to
spare. Held, it opens exactly what SELECT held opens: the display chord and the
hotkey chord. Never forwarded to a core, unmappable (like MENU).

## Decision

Option D. The frontend modifier is **SELECT or HOTKEY**; the GKD reports Home
as HOTKEY. MENU stays instant-open and is still not a modifier. The Brick has
no such key and is unchanged.

## Consequences

- One rule for the player: Home works wherever SELECT works as a modifier.
- The display chord (L1/R1/A) opens on Home too. Whether it should stay a fixed
  chord at all is plorpos-gkd.22 (fold it into the assignable hotkeys).
- The port contract gains a button no core ever sees; `env.c` refuses remaps
  to or from it, the same way it treats MENU.
- The desktop port maps `H` to HOTKEY so the path can be exercised off-device.
- Stick-click as an alternative HOTKEY key is a port-side setting, not a host
  change (plorpos-gkd.21).

## Revisit if

- A device's spare key is one a shipped core needs as a gameplay button.
- plorpos-gkd.22 removes the fixed display chord (this ADR then covers only the
  hotkey chord).

## Revisited 2026-09-30 (plorpos-gkd.22)

The display chord is gone (ADR-0035, revisited). Display mode and filter
are bindable hotkey actions, so Home, like SELECT, now opens only the hotkey
chord. `MODIFIER_BITS` stays where it is, above `hotkey_chord`.

## Revisited 2026-09-30 (plorpos-gkd.43.1)

Home is no longer an alias that is always on beside SELECT. It's one of the
choices for the single modifier the player picks (`modifier=home`), and the
default is MENU. See [ADR-0038](0038-the-hotkey-modifier-is-chosen-default-menu.md).
The stick click (BTN_THUMBL) is reported as `DIATOM_BTN_L3` for the same choice.
