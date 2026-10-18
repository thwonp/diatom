/* SPDX-License-Identifier: MIT */
/* Every core the resident registry can hold gets its own option table
 * (plorpos-a1r): bind one owner per slot, each declares an option, and each
 * must answer with its own value - including after the others have run. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "diatom.h"

static int failures;

#define CHECK(cond, what) do { \
	if (!(cond)) { printf("  FAIL %s\n", what); failures++; } \
} while (0)

void diatom_port_log(diatom_log_level lvl, const char *msg) { (void)lvl; (void)msg; }
void diatom_proto_send(const char *fmt, ...) { (void)fmt; }

/* A core whose definitions die after declaring them, as melonDS DS's do (a
 * stack array, plorpos-3yv.51): built on the heap and freed, so ASan sees any
 * later read of them. */
static void declare_then_free(void)
{
	struct retro_core_option_v2_definition *d = calloc(2, sizeof *d);
	char *key = strdup("ds_opt"), *desc = strdup("Short-lived");
	char *a = strdup("a"), *b = strdup("b");

	d[0].key = key; d[0].desc = desc; d[0].default_value = a;
	d[0].values[0].value = a; d[0].values[1].value = b;
	diatom_options_define_v2(&(struct retro_core_options_v2){ NULL, d });
	free(key); free(desc); free(a); free(b); free(d);
}

int main(void)
{
	static char owners[DIATOM_MAX_RESIDENT];
	char what[64];
	int i;

	for (i = 0; i < DIATOM_MAX_RESIDENT; i++) {
		const struct retro_variable vars[] = {
			{ "core_opt", i % 2 ? "Odd; one|two" : "Even; two|one" },
			{ NULL, NULL },
		};
		const char *v;

		diatom_options_bind(&owners[i]);
		diatom_options_define_vars(vars);
		v = diatom_options_get("core_opt");
		snprintf(what, sizeof what, "core %d of %d has a table", i + 1, DIATOM_MAX_RESIDENT);
		CHECK(v && !strcmp(v, i % 2 ? "one" : "two"), what);
	}
	/* Back to the first: its table is still its own. */
	diatom_options_bind(&owners[0]);
	CHECK(diatom_options_get("core_opt") && !strcmp(diatom_options_get("core_opt"), "two"),
	      "the first core's table survives the others");

	/* Definitions that do not outlive their declaration: the next game's
	 * SETOPTION, and the menu's option list, read only Diatom's copies. */
	diatom_options_bind(&owners[1]);
	declare_then_free();
	CHECK(diatom_options_set("ds_opt", "b"), "an offered value is taken after the core freed its list");
	CHECK(!diatom_options_set("ds_opt", "c"), "an unoffered value is still refused");
	CHECK(!strcmp(diatom_options_get("ds_opt"), "b"), "the value set is the value read");
	diatom_options_emit();
	declare_then_free();             /* a second declaration reuses the slot */
	CHECK(diatom_options_set("ds_opt", "a"), "a redeclared option still offers its values");

	if (failures) { printf("%d check(s) failed\n", failures); return 1; }
	printf("  ok   %d cores, %d tables\n", DIATOM_MAX_RESIDENT, DIATOM_MAX_RESIDENT);
	return 0;
}
