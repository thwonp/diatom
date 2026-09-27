/* NextUI-derived: PolyForm Noncommercial 1.0.0, NOT this repo's root MIT
 * license - see THIRD-PARTY.md before touching this file. */
/* See hotkeys.h. Ported from NextUI's OptionShortcuts_* (ma_frontend_opts.c) -
 * a per-game/per-console cfg file there, four sqlite-backed keys here
 * (TortOS's hotkey.<tag>, mirroring turbo.<tag>'s own precedent, docs/
 * turbo.md), applied over the state plane (SETHOTKEYS) the way turbo's map
 * already rides SETMAP after every RUN.
 *
 * L2/R2/X/Y rather than NextUI's own free choice of button: display_chord
 * (main.c) already claims SELECT+L1/R1/A as the frontend modifier's other
 * chords, and turbo.md's button-budget accounting is exactly why picking
 * from what SELECT does not already claim, rather than repeating NextUI's
 * own list unexamined, was the right call here. */
#include <stdio.h>
#include <string.h>

#include "hotkeys.h"
#include "diatom_port.h"

static struct { int btn; hk_action action; } g_hotkeys[HK_MAX];
static int  g_nhotkeys;
static char g_hotkeys_spec[128];

static int hk_btn_from_name(const char *s)
{
	if (!strcmp(s, "l2")) return DIATOM_BTN_L2;
	if (!strcmp(s, "r2")) return DIATOM_BTN_R2;
	if (!strcmp(s, "x"))  return DIATOM_BTN_X;
	if (!strcmp(s, "y"))  return DIATOM_BTN_Y;
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
	struct { int btn; hk_action action; } parsed[HK_MAX];
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
		int btn;
		hk_action action;
		int i;

		if (!colon || n >= HK_MAX) return false;
		*colon = '\0';
		btn = hk_btn_from_name(tok);
		action = hk_action_from_name(colon + 1);
		if (btn < 0 || action == HK_NONE) return false;
		/* Each button and each action at most once - two rows racing to
		 * drive the same button, or the same action from two buttons, is
		 * exactly the ambiguity turbo.md's button-budget accounting exists
		 * to avoid, just one layer up. */
		for (i = 0; i < n; i++)
			if (parsed[i].btn == btn || parsed[i].action == action)
				return false;
		parsed[n].btn = btn;
		parsed[n].action = action;
		n++;
	}

	memcpy(g_hotkeys, parsed, sizeof parsed);
	g_nhotkeys = n;
	snprintf(g_hotkeys_spec, sizeof g_hotkeys_spec, "%s", spec);
	return true;
}

int hotkeys_count(void) { return g_nhotkeys; }

void hotkeys_at(int i, int *btn_out, hk_action *action_out)
{
	if (i < 0 || i >= g_nhotkeys) { *btn_out = -1; *action_out = HK_NONE; return; }
	*btn_out = g_hotkeys[i].btn;
	*action_out = g_hotkeys[i].action;
}

const char *hotkeys_spec(void) { return g_hotkeys_spec; }

void hotkeys_reset(void)
{
	g_nhotkeys = 0;
	g_hotkeys_spec[0] = '\0';
}
