/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* NextUI-derived: PolyForm Noncommercial 1.0.0, NOT this repo's root MIT
 * license - see NOTICE and THIRD-PARTY.md before touching this file. */
/* The hotkey submenu's binding table (sibling TortOS feature), ported from
 * NextUI's OptionShortcuts_* (ma_frontend_opts.c). Parsing and storage only -
 * see hotkey_chord() in main.c for the per-frame dispatch that actually
 * checks these against the pad each frame.
 *
 * A binding is a trigger and an action. A trigger is an input on one of two
 * layers (ADR-0039): with the modifier held (`x:ff`, the only layer there was
 * before plorpos-gkd.43.2, so a stored spec keeps its meaning) or direct
 * (`d.x:ff`). Directions - the d-pad's and the stick's - are modifier-only,
 * so the d-pad can never stop working for a game.
 *
 * No SDL, no port header, no diatom_core - deliberately, so a test can drive
 * the real parser with no display and no core, the way rewind.c's ring is
 * tested against a fake one. */
#ifndef DIATOM_HOTKEYS_H
#define DIATOM_HOTKEYS_H

#include <stdbool.h>

typedef enum { HK_NONE = 0, HK_FF, HK_REWIND, HK_SAVESTATE, HK_LOADSTATE } hk_action;

#define HK_MAX 4

/* Whole-string validate-then-apply, the same shape as diatom_input_set_map:
 * a spec that is half garbage is refused whole rather than applying the good
 * half - see hotkeys.c for why. On success, replaces the current bindings
 * and what hotkeys_emit()/hotkeys_spec() report; on failure, changes
 * nothing. An empty string is valid and means "no bindings." */
bool hotkeys_set(const char *spec);

/* How many bindings are currently active (0..HK_MAX), and the i'th one -
 * for hotkey_chord() to iterate without reaching into this file's storage
 * directly. `direct_out` is true for a direct trigger, false for one that
 * needs the modifier held. */
int  hotkeys_count(void);
void hotkeys_at(int i, int *btn_out, hk_action *action_out, bool *direct_out);

/* The exact string last accepted by hotkeys_set(), echoed back verbatim by
 * HOTKEYS rather than reserialized from the parsed table - see hotkeys.c. */
const char *hotkeys_spec(void);

/* Back to no bindings - called on every RUN, the same reason SETMAP is reset
 * to identity on every RUN (ADR-0020): a table sent for a different game
 * must not silently keep governing this one. */
void hotkeys_reset(void);

/* The key held for every binding above - one, chosen by the player, MENU by
 * default (plorpos-gkd.43.1, ADR-0038). By name: "menu", "select", "l3",
 * "home" (the GKD's Home).
 * Returns the DIATOM_BTN_* index, or -1 for a name that is not a modifier. */
int  hotkeys_modifier_from_name(const char *name);
void hotkeys_set_modifier(int btn);        /* a value from _from_name */
int  hotkeys_modifier(void);               /* DIATOM_BTN_* */
const char *hotkeys_modifier_name(void);

#endif
