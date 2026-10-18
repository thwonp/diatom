/* SPDX-License-Identifier: MIT */
/* Core options.
 *
 * A core declares what it can be configured with, and asks the frontend for
 * each value on demand. Before this, Diatom accepted the declarations and threw
 * them away, and answered every query with "not set" - so every core ran on its
 * built-in defaults and nothing was adjustable. Measured across the test matrix:
 * 17 to 40 options per core, all of them unreachable.
 *
 * Ownership, in the same split as everything else: **Diatom holds the
 * definitions and the current values; the launcher decides what those values
 * are.** Diatom has no opinion about whether a Genesis should be PAL, and no
 * way to ask - it has no UI (ADR-0009).
 *
 * String lifetime: everything is copied - key, default, description and the
 * value list - as libretro.h asks ("the frontend must maintain its own copy").
 * The description and values were once BORROWED on the reasoning that ADR-0006
 * never unloads a core, so its static data outlives every game. Not every
 * core's definitions are static: melonDS DS builds its array on the stack of
 * the call that declares it (firmware paths found at load), and the next RUN's
 * SETOPTION read a dead frame - the resident died on a second DS game
 * (plorpos-3yv.51).
 *
 * Per-core tables, because a resident process runs many cores and a core asking
 * for "gambatte_gb_colorization" must not be answered from PicoDrive's table.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "diatom.h"

/* One table per core the process has run, claimed on its first RUN and kept
 * for good, like the core itself - so exactly as many as the resident registry
 * can hold. This was a literal 8 that said it matched the registry while the
 * ports set 16, and nine cores ship: the ninth core run in one resident
 * session got "no free table" and ran on its own defaults, ignoring every
 * option the launcher sent (plorpos-a1r). */
#define MAX_TABLES   DIATOM_MAX_RESIDENT
#define MAX_OPTS    96      /* measured worst case is 40; headroom is cheap */
#define MAX_PENDING 32

typedef struct {
	char        key[80];
	char        value[128];         /* current, always one of the core's */
	char        deflt[128];
	char       *pool;               /* owned: desc, then each value, NUL-separated */
	const char *desc;               /* in pool, or NULL */
	const char *values;             /* first value in pool; NULL = no explicit list */
	int         nvalues;
} diatom_option;

/* The value after `v` in an option's pool. */
static const char *next_value(const char *v)
{
	return v + strlen(v) + 1;
}

/* Copy desc and the values into one block the option owns. */
static void own_strings(diatom_option *o, const char *desc,
                        const struct retro_core_option_value *vals, int n)
{
	size_t len = desc ? strlen(desc) + 1 : 0;
	char *p;
	int i;

	for (i = 0; i < n; i++) len += strlen(vals[i].value) + 1;
	if (!len || !(o->pool = malloc(len))) return;
	p = o->pool;
	if (desc) { o->desc = p; p = stpcpy(p, desc) + 1; }
	if (n) o->values = p;
	for (i = 0; i < n; i++) p = stpcpy(p, vals[i].value) + 1;
	o->nvalues = n;
}

typedef struct {
	const void   *owner;            /* the diatom_core this belongs to */
	diatom_option opt[MAX_OPTS];
	int           n;
	bool          dirty;            /* a value changed since the core last asked */
} diatom_option_table;

static diatom_option_table  g_tables[MAX_TABLES];
static diatom_option_table *g_active;

/* Values named before the core has declared anything - the command line is
 * parsed long before retro_set_environment runs. Applied when definitions
 * arrive, so ordering does not matter to the caller. */
static struct { char key[80], value[128]; } g_pending[MAX_PENDING];
static int g_npending;

static void log_(diatom_log_level lvl, const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
	diatom_port_log(lvl, buf);
}

void diatom_options_bind(const void *owner)
{
	int i;

	for (i = 0; i < MAX_TABLES; i++)
		if (g_tables[i].owner == owner) { g_active = &g_tables[i]; return; }
	for (i = 0; i < MAX_TABLES; i++)
		if (!g_tables[i].owner) {
			memset(&g_tables[i], 0, sizeof g_tables[i]);
			g_tables[i].owner = owner;
			g_active = &g_tables[i];
			return;
		}
	log_(DIATOM_LOG_WARN, "options: no free table; options disabled for this core");
	g_active = NULL;
}

static diatom_option *find(const char *key)
{
	int i;
	if (!g_active || !key) return NULL;
	for (i = 0; i < g_active->n; i++)
		if (!strcmp(g_active->opt[i].key, key)) return &g_active->opt[i];
	return NULL;
}

/* A value the caller asked for is only honored if the core offers it. A core
 * handed a value outside its own list is entitled to do anything at all. */
static bool offered(const diatom_option *o, const char *value)
{
	const char *v = o->values;
	int i;
	if (!v) return true;            /* legacy path declared no explicit list */
	for (i = 0; i < o->nvalues; i++, v = next_value(v))
		if (!strcmp(v, value)) return true;
	return false;
}

static void apply_pending(diatom_option *o)
{
	int i;
	for (i = 0; i < g_npending; i++) {
		if (strcmp(g_pending[i].key, o->key)) continue;
		if (!offered(o, g_pending[i].value)) {
			log_(DIATOM_LOG_WARN, "options: %s does not offer '%s'; keeping '%s'",
			     o->key, g_pending[i].value, o->deflt);
			continue;
		}
		snprintf(o->value, sizeof o->value, "%s", g_pending[i].value);
		log_(DIATOM_LOG_INFO, "options: %s = %s", o->key, o->value);
	}
}

static diatom_option *add(const char *key, const char *deflt)
{
	diatom_option *o;

	if (!g_active || !key || !*key) return NULL;
	if (g_active->n >= MAX_OPTS) {
		log_(DIATOM_LOG_WARN, "options: table full at %d, dropping %s", MAX_OPTS, key);
		return NULL;
	}
	o = &g_active->opt[g_active->n++];
	free(o->pool);                  /* the slot's last declaration */
	memset(o, 0, sizeof *o);
	snprintf(o->key, sizeof o->key, "%s", key);
	if (deflt) {
		snprintf(o->deflt, sizeof o->deflt, "%s", deflt);
		snprintf(o->value, sizeof o->value, "%s", deflt);
	}
	return o;
}

void diatom_options_define_v2(const struct retro_core_options_v2 *v2)
{
	const struct retro_core_option_v2_definition *d;

	if (!v2 || !v2->definitions || !g_active) return;
	g_active->n = 0;

	for (d = v2->definitions; d->key; d++) {
		diatom_option *o = add(d->key, d->default_value);
		int i;
		if (!o) break;
		for (i = 0; i < RETRO_NUM_CORE_OPTION_VALUES_MAX && d->values[i].value; i++)
			;
		own_strings(o, d->desc, d->values, i);
		apply_pending(o);
	}
	log_(DIATOM_LOG_INFO, "options: core declared %d", g_active->n);
}

/* The pre-v1 format: value is "Description; first|second|third", and the first
 * listed choice is the default. No core in the test matrix uses it, but it
 * costs a dozen lines and its absence would be a silent loss of configurability
 * rather than a visible failure. */
void diatom_options_define_vars(const struct retro_variable *vars)
{
	const struct retro_variable *v;

	if (!vars || !g_active) return;
	g_active->n = 0;

	for (v = vars; v->key; v++) {
		const char *semi = v->value ? strchr(v->value, ';') : NULL;
		char first[128];
		const char *p, *bar;
		diatom_option *o;

		p = semi ? semi + 1 : NULL;
		while (p && *p == ' ') p++;
		if (!p) { add(v->key, NULL); continue; }
		bar = strchr(p, '|');
		snprintf(first, sizeof first, "%.*s",
		         bar ? (int)(bar - p) : (int)strlen(p), p);
		o = add(v->key, first);
		if (!o) break;
		own_strings(o, v->value, NULL, 0);
		apply_pending(o);
	}
	log_(DIATOM_LOG_INFO, "options: core declared %d (legacy format)", g_active->n);
}

const char *diatom_options_get(const char *key)
{
	diatom_option *o = find(key);
	return (o && o->value[0]) ? o->value : NULL;
}

/* Forget every value the launcher asked for. Called when a game ends, so the
 * next one starts from the core's own defaults plus whatever the launcher sends
 * for THAT game.
 *
 * Without this they were permanent. A core re-declares its options on every
 * load and `define_v2` rebuilds the table at those defaults, so the only thing
 * carrying a value across launches was this list - and it carried it to every
 * core that ever declared the same key. Measured 2026-08-28: TortOS pins
 * mgba_gb_model=Game Boy for its Game Boy folder, and Game Boy COLOR titles
 * launched afterwards came up in DMG green, because the same mgba serves both
 * and the pending value outlived the game it was for. Dragon Warrior III is a
 * 0xC0 cartridge and showed its own "only for Game Boy Color" warning screen.
 *
 * Per-launch is also the right model rather than merely the fixed one: ADR-0009
 * makes the launcher drive, and an option it set for one game is not a standing
 * instruction about the next. */
void diatom_options_clear_pending(void)
{
	g_npending = 0;
}

bool diatom_options_set(const char *key, const char *value)
{
	diatom_option *o = find(key);

	if (!key || !value) return false;

	/* Recorded as INTENT, always, whether or not the core has declared this key
	 * yet. The table below is the core's current state; this list is what the
	 * caller asked for, and the two are not the same thing.
	 *
	 * Both halves of that were bugs, found on 2026-08-28. Writing only the
	 * table lost the value: a core re-declares its options on every
	 * retro_load_game and define_v2 rebuilds the table at its defaults, so an
	 * option set on a resident core was wiped by the very load it was sent
	 * for, and a launcher's setting worked exactly once per core per process.
	 * Writing only the list, appended, leaked the other way - a launcher
	 * stating its preferences before every launch added an entry each time and
	 * was refused on the 33rd.
	 *
	 * So: keyed, and dropped when a game ends by
	 * diatom_options_clear_pending(). Intent survives the core re-declaring
	 * and does not survive the game it was for, which is what kept
	 * mgba_gb_model=Game Boy from a Game Boy launch and applied it to the Game
	 * Boy COLOR title after it - same core, different machine. */
	{
		int i;
		for (i = 0; i < g_npending; i++)
			if (!strcmp(g_pending[i].key, key)) break;
		if (i == g_npending) {
			if (g_npending >= MAX_PENDING) return false;
			snprintf(g_pending[i].key, sizeof g_pending[0].key, "%s", key);
			g_npending++;
		}
		snprintf(g_pending[i].value, sizeof g_pending[0].value, "%s", value);
	}

	if (!o) return true;             /* not declared yet; apply_pending will */
	if (!offered(o, value)) {
		log_(DIATOM_LOG_WARN, "options: %s does not offer '%s'", key, value);
		return false;
	}
	if (strcmp(o->value, value)) {
		snprintf(o->value, sizeof o->value, "%s", value);
		g_active->dirty = true;
	}
	return true;
}

/* Cores poll this every frame - all six measured do - so it must stay cheap and
 * must report a change exactly once. */
bool diatom_options_take_update(void)
{
	bool d;
	if (!g_active) return false;
	d = g_active->dirty;
	g_active->dirty = false;
	return d;
}

int diatom_options_count(void) { return g_active ? g_active->n : 0; }

/* One line per option, for a launcher building a menu. Values are pipe-joined
 * because the protocol is tab-separated, so a tab inside a field would be
 * indistinguishable from the end of it. */
void diatom_options_emit(void)
{
	int i, j;

	if (!g_active) { diatom_proto_send("OPTIONS\tcount=0"); return; }
	diatom_proto_send("OPTIONS\tcount=%d", g_active->n);
	for (i = 0; i < g_active->n; i++) {
		const diatom_option *o = &g_active->opt[i];
		const char *v = o->values;
		char vals[512];
		size_t used = 0;

		vals[0] = '\0';
		for (j = 0; j < o->nvalues && used < sizeof vals - 1; j++, v = next_value(v)) {
			int n = snprintf(vals + used, sizeof vals - used, "%s%s",
			                 used ? "|" : "", v);
			if (n < 0) break;
			used += (size_t)n;
		}
		diatom_proto_send("OPTION\tkey=%s\tvalue=%s\tdefault=%s\tvalues=%s\tdesc=%s",
		                  o->key, o->value, o->deflt, vals,
		                  o->desc ? o->desc : "");
	}
}

void diatom_options_list(void)
{
	int i, j;

	if (!g_active) { printf("diatom: no options table\n"); return; }
	printf("diatom: %d core option(s)\n", g_active->n);
	for (i = 0; i < g_active->n; i++) {
		const diatom_option *o = &g_active->opt[i];
		const char *v = o->values;

		printf("  %-40s = %-20s %s\n", o->key, o->value,
		       o->desc ? o->desc : "");
		if (!v) continue;
		printf("      ");
		for (j = 0; j < o->nvalues; j++, v = next_value(v))
			printf("%s%s", j ? " | " : "", v);
		printf("\n");
	}
}
