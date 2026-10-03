/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* NextUI-derived: PolyForm Noncommercial 1.0.0, NOT this repo's root MIT
 * license - see NOTICE and THIRD-PARTY.md before touching this file. */
/* See hotkeys.h. Ported from NextUI's OptionShortcuts_* (ma_frontend_opts.c) -
 * a per-game/per-console cfg file there, four sqlite-backed keys here
 * (TortOS's hotkey.<tag>, mirroring turbo.<tag>'s own precedent, docs/
 * turbo.md), applied over the state plane (SETHOTKEYS) the way turbo's map
 * already rides SETMAP after every RUN.
 *
 * Triggers: the face buttons and shoulders (L1/R1/L2/R2/A/B/X/Y) on either
 * layer, and the d-pad's and stick's directions with the modifier only
 * (ADR-0039). START and the modifier itself stay out. L1/R1/A joined when
 * plorpos-gkd.22 retired the fixed display chord that used to own them.
 * Display mode and filter were bindable actions here from then until
 * plorpos-gkd.73: the mode is the launcher's menus' job, and the filter's
 * only other look is plain bilinear on the GKD, which nobody wanted. */
#include <stdio.h>
#include <string.h>

#include "hotkeys.h"
#include "diatom_port.h"

typedef struct { int btn; hk_action action; bool direct; } hk_binding;

static hk_binding g_hotkeys[HK_MAX];
static int  g_nhotkeys;
static char g_hotkeys_spec[128];
static int  g_modifier = DIATOM_BTN_MENU;

static const struct { const char *name; int btn; } g_modifiers[] = {
	{ "menu",   DIATOM_BTN_MENU   },
	{ "select", DIATOM_BTN_SELECT },
	{ "l3",     DIATOM_BTN_L3     },
	{ "home",   DIATOM_BTN_HOTKEY },   /* the GKD's Home, ADR-0037 */
};

/* `direct` says whether the input may be a direct trigger. Directions may
 * not: a direct binding hides its input from the game, and the d-pad (the
 * stick folds onto it) must never stop working. */
static const struct { const char *name; int btn; bool direct; } g_inputs[] = {
	{ "l1", DIATOM_BTN_L1, true }, { "r1", DIATOM_BTN_R1, true },
	{ "l2", DIATOM_BTN_L2, true }, { "r2", DIATOM_BTN_R2, true },
	{ "a",  DIATOM_BTN_A,  true }, { "b",  DIATOM_BTN_B,  true },
	{ "x",  DIATOM_BTN_X,  true }, { "y",  DIATOM_BTN_Y,  true },
	{ "up",     DIATOM_BTN_UP,     false }, { "down",   DIATOM_BTN_DOWN,   false },
	{ "left",   DIATOM_BTN_LEFT,   false }, { "right",  DIATOM_BTN_RIGHT,  false },
	{ "sup",    DIATOM_BTN_SUP,    false }, { "sdown",  DIATOM_BTN_SDOWN,  false },
	{ "sleft",  DIATOM_BTN_SLEFT,  false }, { "sright", DIATOM_BTN_SRIGHT, false },
};

static int hk_btn_from_name(const char *s, bool direct)
{
	size_t i;

	for (i = 0; i < sizeof g_inputs / sizeof g_inputs[0]; i++)
		if (!strcmp(s, g_inputs[i].name))
			return direct && !g_inputs[i].direct ? -1 : g_inputs[i].btn;
	return -1;
}

static hk_action hk_action_from_name(const char *s)
{
	if (!strcmp(s, "ff"))        return HK_FF;
	if (!strcmp(s, "rewind"))    return HK_REWIND;
	if (!strcmp(s, "savestate")) return HK_SAVESTATE;
	if (!strcmp(s, "loadstate")) return HK_LOADSTATE;
	return HK_NONE;
}

bool hotkeys_set(const char *spec)
{
	hk_binding parsed[HK_MAX];
	int n = 0;
	char buf[128], *save = NULL, *tok;

	if (!spec) return false;
	if (!*spec) {
		g_nhotkeys = 0;
		g_hotkeys_spec[0] = '\0';
		return true;
	}
	if (strlen(spec) >= sizeof buf) return false;
	snprintf(buf, sizeof buf, "%s", spec);

	for (tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
		char *colon = strchr(tok, ':');
		bool direct = !strncmp(tok, "d.", 2);
		int btn;
		hk_action action;
		int i;

		if (!colon) return false;
		/* Retired by plorpos-gkd.73, still in specs saved before it. Skipped
		 * rather than refused, or one stale entry would cost that system
		 * every binding it has. Before the HK_MAX check: a full set of four
		 * plus a stale one is still four. */
		if (!strcmp(colon + 1, "display") || !strcmp(colon + 1, "filter"))
			continue;
		if (n >= HK_MAX) return false;
		*colon = '\0';
		btn = hk_btn_from_name(direct ? tok + 2 : tok, direct);
		action = hk_action_from_name(colon + 1);
		if (btn < 0 || action == HK_NONE) return false;
		/* Each trigger and each action at most once - two rows racing to
		 * drive the same trigger, or the same action from two triggers, is
		 * exactly the ambiguity turbo.md's button-budget accounting exists
		 * to avoid, just one layer up. X and d.X are two triggers: a button
		 * may hold one binding on each layer. */
		for (i = 0; i < n; i++)
			if ((parsed[i].btn == btn && parsed[i].direct == direct)
			    || parsed[i].action == action)
				return false;
		parsed[n].btn = btn;
		parsed[n].action = action;
		parsed[n].direct = direct;
		n++;
	}

	memcpy(g_hotkeys, parsed, sizeof parsed);
	g_nhotkeys = n;
	snprintf(g_hotkeys_spec, sizeof g_hotkeys_spec, "%s", spec);
	return true;
}

int hotkeys_count(void) { return g_nhotkeys; }

void hotkeys_at(int i, int *btn_out, hk_action *action_out, bool *direct_out)
{
	if (i < 0 || i >= g_nhotkeys) {
		*btn_out = -1; *action_out = HK_NONE; *direct_out = false;
		return;
	}
	*btn_out = g_hotkeys[i].btn;
	*action_out = g_hotkeys[i].action;
	*direct_out = g_hotkeys[i].direct;
}

const char *hotkeys_spec(void) { return g_hotkeys_spec; }

void hotkeys_reset(void)
{
	g_nhotkeys = 0;
	g_hotkeys_spec[0] = '\0';
	g_modifier = DIATOM_BTN_MENU;
}

int hotkeys_modifier_from_name(const char *name)
{
	size_t i;

	for (i = 0; name && i < sizeof g_modifiers / sizeof g_modifiers[0]; i++)
		if (!strcmp(name, g_modifiers[i].name)) return g_modifiers[i].btn;
	return -1;
}

void hotkeys_set_modifier(int btn) { if (btn >= 0) g_modifier = btn; }
int  hotkeys_modifier(void) { return g_modifier; }

const char *hotkeys_modifier_name(void)
{
	size_t i;

	for (i = 0; i < sizeof g_modifiers / sizeof g_modifiers[0]; i++)
		if (g_modifiers[i].btn == g_modifier) return g_modifiers[i].name;
	return "menu";
}
