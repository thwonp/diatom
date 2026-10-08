/* SPDX-License-Identifier: MIT */
/* Every core the resident registry can hold gets its own option table
 * (plorpos-a1r): bind one owner per slot, each declares an option, and each
 * must answer with its own value - including after the others have run. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "diatom.h"

static int failures;

#define CHECK(cond, what) do { \
	if (!(cond)) { printf("  FAIL %s\n", what); failures++; } \
} while (0)

void diatom_port_log(diatom_log_level lvl, const char *msg) { (void)lvl; (void)msg; }
void diatom_proto_send(const char *fmt, ...) { (void)fmt; }

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

	if (failures) { printf("%d check(s) failed\n", failures); return 1; }
	printf("  ok   %d cores, %d tables\n", DIATOM_MAX_RESIDENT, DIATOM_MAX_RESIDENT);
	return 0;
}
