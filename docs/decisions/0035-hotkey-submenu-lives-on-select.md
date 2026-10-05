# 0035. The hotkey submenu is a SELECT-held chord Diatom checks itself

- **Status:** Accepted
- **Date:** 2026-09-27 (accepted 2026-09-27)
- **Supersedes:** -
- **Superseded by:** [ADR-0037](0037-home-is-a-second-modifier-key.md), in part (the modifier is SELECT or HOTKEY; MENU is still not a modifier)

Extends [ADR-0020](0020-shared-state-plane.md) and sits directly beside
[ADR-0034](0034-fast-forward-and-rewind.md), whose `g_ff_speed`/
`g_rewind_active` this is the primary real-time driver of. Also the first
concrete answer to a question [ADR-0014](0014-display-modes-and-default.md)
left open (register §5, the `[DEFERRED]` display-chord item: "ADR-0014 leaves
hotkey ownership open").

## Context

TortOS's sibling feature is a per-action, player-configurable rebinding
screen: which button, if any, triggers fast-forward, rewind, a quicksave, a
quickload. Ported in spirit from NextUI's `OptionShortcuts_*`
(`ma_frontend_opts.c`), not copied - NextUI's shape does not fit here, for
reasons specific to this frontend:

1. **Diatom already has a working precedent for exactly this shape of thing:**
   `display_chord()` (`src/main.c`) is a SELECT-held modifier chord, checked
   every frame, that claims L1/R1/A for frontend actions (display mode,
   filter) and hides them from the core while SELECT is down. It predates
   this ADR and was built for display-mode comparison, not for hotkeys - but
   it is the only place in this codebase that has ever done "a button means
   something to the frontend instead of the game," and it works.
2. **Gameplay input is polled by Diatom, not the launcher.** The launcher
   process is blocked on the protocol socket for the whole session (ADR-0008);
   Diatom's own frame loop is the only thing that ever sees a per-frame button
   state while a game is actually running. A hotkey that must fire the instant
   a button goes down during live play - not only while the launcher's own
   paused menu has input focus - has to be detected here, not there.
3. **MENU is not available as a modifier.** It already means "open the
   launcher's pause menu," edge-triggered on press with no delay. Making it
   double as a hold-modifier for hotkeys would mean deciding, on every press,
   whether to wait and see if another button follows before opening the menu -
   a real, felt latency regression on a control that opens instantly today,
   and one nothing in this environment could measure to confirm was actually
   fine. Changing working, load-bearing MENU behavior on a guess was rejected
   on those grounds alone.
4. **The four candidate actions need very different handling.** Fast-forward
   and rewind are already levels this session's frame loop drives every
   frame (`g_ff_speed`, `g_rewind_active` - ADR-0034); a hotkey for either is
   naturally level-triggered too - held means on. Save/load state are one-shot:
   edge-triggered, firing once per press, against `sn->state_exit` - the same
   Auto-slot path the launcher already hands over at RUN (ADR-0024), autosaves
   to, and its own pause-menu Save/Load-to-Auto row already targets. Nothing
   new to pass over the protocol for either.
5. **Button budget is real and already has a table.** `docs/turbo.md`'s whole
   subject is how few spare face buttons exist per system - 0 on SNES, 1 on
   Genesis. A hotkey chord that requires SELECT held first never collides with
   plain gameplay input or with turbo's own plain-button remapping (ADR-0028),
   because neither of those ever has SELECT down at the same time. This is the
   same property that already makes `display_chord` safe on every system with
   no per-system exception table - inherited, not re-derived.

## Options considered

### Option A - launcher-side detection, driven over the existing protocol

The launcher polls its own input while paused and sends `SETSPEED`/
`SETREWIND`/`SAVE`/`LOAD` (all of which already exist) when a bound button is
pressed.

Rejected by fact 2: the launcher is not reading input while a game is
actually running, only while its own menu already has focus. This would make
hotkeys work only from inside the pause menu, which is not a hotkey - it is
the existing menu with extra rows, and the sibling feature explicitly wants
both (the menu rows AND a live, in-game shortcut).

### Option B - MENU-held chord, matching NextUI's own convention literally

`bind Save State = MENU+X`, ported as written.

Rejected by fact 3. NextUI's MENU is not TortOS's MENU: minarch's menu-open
key has no equivalent latency concern this frontend's own MENU does, because
nothing else here established MENU as instant-open, tested, and relied upon.
Porting the binding *value* without checking whether the modifier it depends
on means the same thing here was the trap - the sourcing permission covers
code, not the assumption that every button means what NextUI's docs say it
means.

### Option C - a new, dedicated hotkey modifier key

Reserve a button (say START, unused as a modifier today) purely for hotkeys,
separate from SELECT's existing display chord.

Rejected: two frontend modifier keys where fact 1 already shows one works is
an unforced second thing to remember, test, and explain, for no capability
Option D does not already have. Nothing about hotkeys needs its own modifier;
they need a modifier that is not MENU, and SELECT already qualifies.

### Option D - extend `display_chord`'s existing SELECT convention

New candidate buttons (L2, R2, X, Y - chosen as exactly what SELECT+L1/R1/A
does not already claim) for four new frontend actions, checked by a sibling
function (`hotkey_chord`) on the same terms, composed into the same
suppression mask the way `display_chord` and `held_at_entry` already are.

Chosen.

## Decision

**Option D. Hotkeys are a SELECT-held chord, detected inside Diatom's own
frame loop, on candidate buttons `display_chord` does not already use.**

```
launcher → SETHOTKEYS hotkeys=l2:ff,r2:rewind,x:savestate,y:loadstate
launcher → HOTKEYS                query
Diatom   → HOTKEYS hotkeys=...    the reply; never sent unsolicited
```

ADR-0020's ownership table gains a row:

| State | Defined by | Written by | Diatom emits unsolicited |
|---|---|---|---|
| **Hotkeys** | **launcher** | **launcher** | **no** |

- **Candidate buttons: L2, R2, X, Y only.** Not a free choice of any button,
  and not NextUI's own list - specifically what `display_chord` leaves
  unclaimed under SELECT (L1, R1, A are its), so the two chord sets can never
  disagree about what a press under SELECT means.
- **Four actions, fixed:** `ff`, `rewind`, `savestate`, `loadstate`. Not
  arbitrary or extensible from the wire format - `reset` and anything else
  NextUI's `SHORTCUT_*` enum has stayed out on purpose; nothing on the TortOS
  side asked for them yet and a fifth action is a small, additive change
  whenever one does.
- **Parsing and storage are their own file, `src/hotkeys.c`/`.h`, deliberately
  split out of `main.c`** (unlike `display_chord`, which stays there) so the
  validate-then-apply logic - whole-string refusal on any bad button, bad
  action, or a button/action claimed twice, the same shape
  `diatom_input_set_map` already uses - can be exercised by a test with no
  SDL, no core, and no display. `hotkey_chord()` itself, which touches
  `g_ff_speed`/`g_rewind_active`/`g_core` directly, stays in `main.c` next to
  `display_chord` for the same reason that one does.
- **Reset to no bindings on every `RUN`,** matching `SETMAP`'s own reset
  (ADR-0020): a table sent for a different game must not silently keep
  governing this one. The launcher re-sends `SETHOTKEYS` after `RUN` on the
  same terms it already re-sends turbo's `SETMAP`.
- **A hotkey only ever undoes what a hotkey itself turned on.**
  `g_hotkey_ff_active`/`g_hotkey_rewind_active` track this per action, so
  releasing a fast-forward hotkey never stomps a value some other caller
  raised `g_ff_speed` to for an unrelated reason - nothing does that today,
  but the guard cost one bool each.
- **Verification ceiling, stated plainly:** the parser (`hotkeys.c`) has a
  real, SDL-free test covering the accept path, the empty-clears-everything
  path, and every refusal shape (bad button, bad action, missing separator,
  a button or action claimed twice, more than four entries) - all pass. The
  wire format (`SETHOTKEYS`/`HOTKEYS`) is tested the same way ADR-0034's
  verbs are, over a real socket. `hotkey_chord()`'s actual per-frame
  dispatch - suppression, the level/edge split, the interaction with a real
  core's save/load state - is reviewed, not executed: no core, no ROM, and no
  way to build `$(BIN)` were available in the environment this was written
  in.

## Consequences

**Easier.** A genuinely configurable hotkey exists without a second
modifier key, without touching MENU, and without a new per-frame poll site -
it rides the one `display_chord` already pays for.

**Harder.** SELECT now has eight meanings under one held key (four of
`display_chord`'s own, four of these) for whoever next reads `main.c`
wondering what SELECT does. Worth a code comment pointer between the two
functions if a ninth is ever added.

**Forecloses (for now):** TOGGLE-style hotkeys (press once, stays on until
pressed again) - NextUI has both `HOLD_*` and `TOGGLE_*` variants; this ships
only the held kind, which needs no persistent latch. A toggle variant is an
additive change to `hotkey_chord`'s action enum, not a redesign, whenever one
is asked for.

## Revisit if

- A fifth hotkey action is wanted - extend the `hk_action` enum and
  `hk_action_from_name` in `hotkeys.c`; nothing else in this design assumes
  exactly four.
- SELECT's chord budget (this ADR's four buttons plus `display_chord`'s
  three) is ever felt to be too much to hold in one's head - the register's
  own `[DEFERRED]` display-chord item (§5) already flags `display_chord`
  itself for removal once its original diagnostic purpose is done; if that
  happens first, this ADR's chords become SELECT's only ones and the
  crowding concern above goes away on its own.
- Real hardware shows the one-frame gap between a hotkey's edge and a core
  that already read this frame's input causes a missed save/load in
  practice (the same leak `display_chord`'s own comment already names and
  accepts for its own chords).

## Revisited 2026-09-30 (plorpos-gkd.22)

The `[DEFERRED]` removal above happened: `display_chord` is gone, and its
three fixed chords became two bindable actions in the same table.

- **Actions:** `display` (next display mode, wrapping) and `filter`
  (sharp/nearest), both edge-triggered like `savestate`/`loadstate`. There is no
  "previous mode". The player asked for one looping action instead of the
  chord's L1/R1 pair. `HK_MAX` is 6.
- **Buttons:** L1, R1, L2, R2, A, B, X, Y. L1/R1/A are free now that the chord
  is gone, and B joined because SELECT is held for all of them anyway.
  START, the d-pad and SELECT stay the game's.
- **No defaults**, as before: a system with no `hotkey.<tag>` row gets no
  display or filter chord either. Standalone Diatom (no launcher, so no
  SETHOTKEYS) can no longer change the mode from the pad.
- The "eight meanings under SELECT" crowding concern resolves the way
  Revisit-if predicted: this table is SELECT's only chord source.
- No `proto=` bump: `display`/`filter` are new values of an existing
  field, and the launcher and Diatom ship together.

## Revisited 2026-09-30 (plorpos-gkd.43.1)

SELECT is no longer the fixed modifier. The player chooses it, and the default is MENU.
See [ADR-0038](0038-the-hotkey-modifier-is-chosen-default-menu.md).

## Revisited 2026-10-02 (plorpos-gkd.73)

`display` and `filter` are no longer actions; `HK_MAX` is 4 (ff, rewind,
savestate, loadstate). The display mode is set from the launcher's shelf and
in-game menus, which was enough. The filter's other value is plain bilinear on
the GKD (SDL has no sharp-bilinear), which read as blur, so nearest stays the
only filter until shaders (plorpos-gkd.72) bring a real sharp look.

A spec saved before this still carries `display`/`filter` entries, and the
launcher sends saved specs as they are. Those two names are skipped, not
refused, so a stale entry cannot cost a system its other bindings. Any other
unknown action is still refused whole. `--filter` and SETDISPLAY's `filter=`
are unchanged.

GKD branch only (feature/gkd). On the Brick, `sharp` is a real sharp-bilinear
and the hotkey is its only way in, so dev keeps both actions.

## Revisited 2026-10-04 (plorpos-gkd.86.2)

`screenshot` is a fifth action; `HK_MAX` is 5. Edge-triggered like
`savestate`: the press grabs what is on glass (`diatom_port_grab` - the
display's size, its scaling and shader, no overlay) and a thread of its own
writes `<shots>/<rom's name>-YYYYMMDD-HHMMSS.png`, so the game stops only for
the grab (~30 ms on the GKD at 1600x1440), not the encode (~0.35-1 s). A press
while the last one is still being written is dropped and logged. The folder is
RUN's `shots=` (ADR-0024's rule: the launcher says where things go) or
`--shots`; Diatom makes it if it is missing. With neither, the action does
nothing. No `proto=` bump: a new value of an existing field and a new optional
RUN key, and the launcher and Diatom ship together.
