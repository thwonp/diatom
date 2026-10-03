/* SPDX-License-Identifier: MIT */
/* Loading a core, and its lifecycle.
 *
 * ADR-0006: every core is dlopen'd once and NEVER unloaded. dlclose does not
 * appear in this file, or anywhere else in Diatom.
 * ADR-0010: RTLD_LOCAL always. It is the only thing preventing symbol collision
 * between resident cores - measured, picodrive exports 1069 non-retro_* symbols
 * including a complete static zlib. RTLD_GLOBAL would let its crc32 answer for
 * everyone, silently.
 */
#include <dirent.h>
#include <dlfcn.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cheevos.h"
#include "diatom.h"

static bool bind_sym(void *h, void *slot, const char *name)
{
	void *p = dlsym(h, name);
	if (!p) {
		fprintf(stderr, "diatom: core is missing %s\n", name);
		return false;
	}
	memcpy(slot, &p, sizeof p);
	return true;
}

#define BIND(field, name) \
	if (!bind_sym(c->handle, &c->field, name)) return false

/* What a core costs to open, logged rather than remembered.
 *
 * Timed separately because they are separate costs and only one of them is
 * large: measured on the device 2026-09-08, a COLD dlopen runs 10-179 ms
 * depending on the core - genesis_plus_gx is the big one - against 0.8-26 ms
 * warm, while retro_init is under 3 ms for five of the six. The README's
 * "~170 ms" was right and never said it was cold, which is how it read as
 * wrong for a year.
 *
 * Kept because these are the numbers that decide whether premapping is worth
 * doing, and they will drift when a core is repinned. */
static double dbg_now_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

static const char *dbg_base(const char *p)
{
	const char *s = strrchr(p, '/');
	return s ? s + 1 : p;
}

bool diatom_core_open(diatom_core *c, const char *path)
{
	double t0;

	memset(c, 0, sizeof *c);
	c->path = path;

	t0 = dbg_now_ms();
	c->handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);   /* ADR-0010 */
	fprintf(stderr, "diatom: dlopen %-28s %6.1f ms\n",
	        dbg_base(path), dbg_now_ms() - t0);
	if (!c->handle) {
		fprintf(stderr, "diatom: dlopen %s: %s\n", path, dlerror());
		return false;
	}

	BIND(set_environment,            "retro_set_environment");
	BIND(set_video_refresh,          "retro_set_video_refresh");
	BIND(set_audio_sample,           "retro_set_audio_sample");
	BIND(set_audio_sample_batch,     "retro_set_audio_sample_batch");
	BIND(set_input_poll,             "retro_set_input_poll");
	BIND(set_input_state,            "retro_set_input_state");
	BIND(set_controller_port_device, "retro_set_controller_port_device");
	BIND(init,                       "retro_init");
	BIND(deinit,                     "retro_deinit");
	BIND(get_system_info,            "retro_get_system_info");
	BIND(get_system_av_info,         "retro_get_system_av_info");
	BIND(load_game,                  "retro_load_game");
	BIND(unload_game,                "retro_unload_game");
	BIND(run,                        "retro_run");
	BIND(reset,                      "retro_reset");
	BIND(serialize_size,             "retro_serialize_size");
	BIND(serialize,                  "retro_serialize");
	BIND(unserialize,                "retro_unserialize");
	BIND(get_memory_data,            "retro_get_memory_data");
	BIND(get_memory_size,            "retro_get_memory_size");

	return true;
}

/* Resident cores - ADR-0006.
 *
 * Diatom never calls dlclose, so a core opened once is opened forever. This
 * registry is what makes that true across games: the second RUN naming the same
 * core skips dlopen and retro_init entirely, which measured 6 ms and 43 ms
 * respectively for FCEUmm and is most of what a warm launch saves.
 *
 * Bounded rather than dynamic because the bound is the point: six cores mapped
 * plus one running measured 15.0 MB against 975 MB of RAM, and a registry that
 * grows without limit would quietly turn a measured decision into an unmeasured
 * one. The bound is per port: 8 is the Brick's, measured; a port with the
 * memory to spare sets DIATOM_MAX_RESIDENT from the Makefile.
 */
#ifndef DIATOM_MAX_RESIDENT
#define DIATOM_MAX_RESIDENT 8
#endif
#define MAX_RESIDENT DIATOM_MAX_RESIDENT
static diatom_core g_resident[MAX_RESIDENT];
static char        g_resident_path[MAX_RESIDENT][1024];
static int         g_nresident;

diatom_core *diatom_core_resident(const char *path)
{
	int i;

	if (!path || !*path) return NULL;
	for (i = 0; i < g_nresident; i++)
		if (!strcmp(g_resident_path[i], path))
			return &g_resident[i];

	if (g_nresident >= MAX_RESIDENT) {
		fprintf(stderr, "diatom: core registry full (%d)\n", MAX_RESIDENT);
		return NULL;
	}
	if (!diatom_core_open(&g_resident[g_nresident], path)) return NULL;

	/* The caller's path buffer is reused between messages, so own a copy. */
	snprintf(g_resident_path[g_nresident], sizeof g_resident_path[0], "%s", path);
	g_resident[g_nresident].path = g_resident_path[g_nresident];
	return &g_resident[g_nresident++];
}

int diatom_core_resident_count(void) { return g_nresident; }

/* Map every core in `dir` before anyone asks for one.
 *
 * launch.sh used to warm the page cache by reading the cores with cat during
 * the boot animation - 480 ms cold to read every byte, and it bought only the
 * I/O. This does the same work in the same idle seconds for less: dlopen maps
 * what it needs rather than reading the file through, measured 382 ms cold,
 * and it also pays the dynamic linker so a first launch does not.
 *
 * Called before the socket exists, so the launcher either finds no socket and
 * runs a game standalone - which it is already built to do in the first second
 * after boot - or finds one with every core ready. It never finds a socket
 * that answers slowly.
 *
 * A core that fails here is not fatal: it will be tried again by name when a
 * RUN asks for it, and fail there with the launcher listening. */
int diatom_core_premap(const char *dir)
{
	DIR *d = opendir(dir);
	struct dirent *e;
	int n = 0;

	if (!d) return 0;
	while ((e = readdir(d))) {
		char path[1024];
		size_t len = strlen(e->d_name);

		if (len < 12 || strcmp(e->d_name + len - 12, "_libretro.so") != 0)
			continue;
		snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
		if (diatom_core_resident(path)) n++;
	}
	closedir(d);
	return n;
}

/* Cores that set need_fullpath want a path and read the file themselves;
 * the rest want the bytes. Getting this backwards is a silent load failure. */
static bool read_file(const char *path, void **out, size_t *len)
{
	FILE *f = fopen(path, "rb");
	long n;
	void *buf;

	*out = NULL; *len = 0;
	if (!f) return false;
	if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return false; }
	n = ftell(f);
	if (n <= 0) { fclose(f); return false; }
	rewind(f);

	buf = malloc((size_t)n);
	if (!buf) { fclose(f); return false; }
	if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
		free(buf); fclose(f); return false;
	}
	fclose(f);
	*out = buf; *len = (size_t)n;
	return true;
}

bool diatom_core_start(diatom_core *c, const char *rom_path, int disc)
{
	struct retro_system_info si;
	struct retro_game_info gi;
	void *data = NULL;
	size_t len = 0;
	bool ok;

	memset(&si, 0, sizeof si);
	c->get_system_info(&si);

	/* Before the core can declare anything. A core re-declares its memory map
	 * on every load, and a pointer kept from the previous game points into
	 * memory that core has since freed - ADR-0025. */
	diatom_cheevos_reset();
	/* Same reason, one declaration further: forget the last game's pixel
	 * format so this load's silence can be told from its agreement. */
	diatom_env_pixfmt_begin();

	if (!c->initialized) {
		double t0 = dbg_now_ms();

		c->init();
		c->initialized = true;
		fprintf(stderr, "diatom: retro_init %-25s %6.1f ms\n",
		        dbg_base(c->path), dbg_now_ms() - t0);
	}

	/* A core that declared SET_SUPPORT_NO_GAME expects NULL, not a path. */
	if (!rom_path) {
		if (!c->load_game(NULL)) {
			fprintf(stderr, "diatom: core refused to start without content\n");
			return false;
		}
		c->game_loaded = true;
		diatom_env_pixfmt_settle(c);
		diatom_cheevos_resolve(c);
		return true;
	}

	memset(&gi, 0, sizeof gi);
	gi.path = rom_path;

	/* Zipped content - how a launcher's library actually arrives. The ROM is
	 * extracted here rather than by the core: of the pinned set only some
	 * cores unzip for themselves, and the first real launcher found that out
	 * on the first game it handed over. One entry per archive (the largest);
	 * a need_fullpath core gets the extraction as a tmpfs file, since it
	 * wants to read from disk, and everything else gets the buffer.
	 *
	 * Except a core that sets block_extract, which says the archive IS the
	 * content: an arcade set is identified by its zip's name, and FBNeo handed
	 * one ROM out of 1943.zip ("bme02.13d") reports the romset as unknown.
	 * Such a core gets the zip path as is (plorpos-gkd.56). */
	if (diatom_zip_is(rom_path) && !si.block_extract) {
		char inner[512];

		if (!diatom_zip_load(rom_path, &data, &len, inner, sizeof inner)) {
			fprintf(stderr, "diatom: cannot extract %s\n", rom_path);
			return false;
		}
		fprintf(stderr, "diatom: zip: %s -> %s (%zu bytes)\n",
		        rom_path, inner, len);
		if (si.need_fullpath) {
			static char tmp[600];
			FILE *tf;
			const char *base = strrchr(inner, '/');

			snprintf(tmp, sizeof tmp, "/tmp/diatom-%s",
			         base ? base + 1 : inner);
			tf = fopen(tmp, "wb");
			if (!tf || fwrite(data, 1, len, tf) != len) {
				if (tf) fclose(tf);
				free(data);
				fprintf(stderr, "diatom: cannot stage %s\n", tmp);
				return false;
			}
			fclose(tf);
			free(data);
			data = NULL;
			gi.path = tmp;
		} else {
			gi.data = data;
			gi.size = len;
		}
	} else if (!si.need_fullpath) {
		if (!read_file(rom_path, &data, &len)) {
			fprintf(stderr, "diatom: cannot read %s\n", rom_path);
			return false;
		}
		gi.data = data;
		gi.size = len;
	}

	c->disk_close_in = 0;
	ok = c->load_game(&gi);

	/* Held until the game unloads, as RetroArch holds it. Most cores copy the
	 * bytes inside retro_load_game, but fake08 only queues the pointer and
	 * reads the cart on its first retro_run: freed here, that read landed in
	 * freed memory and segfaulted (plorpos-gkd.50.1). The cost is one copy of
	 * a cartridge ROM while it plays; disc and arcade cores are need_fullpath
	 * and never get a buffer. */
	if (!ok) {
		free(data);
		fprintf(stderr, "diatom: core refused %s\n", rom_path);
		return false;
	}
	c->content = data;
	c->game_loaded = true;
	if (c->has_disk) {
		fprintf(stderr, "diatom: disk: interface v%d, %u images, on %u\n",
		        c->disk_ext ? 1 : 0, c->disk.get_num_images(),
		        c->disk.get_image_index());
		/* The disc a resumed game was last on, plorpos-gkd.47, so a state
		 * saved on disc 2 finds disc 2 in the drive. Swapped here, before
		 * the first frame, rather than through EXT's set_initial_image:
		 * pcsx_rearmed honours that only for the path of the image itself
		 * as it spells it (<m3u dir>/<line>), which a frontend can learn
		 * only from get_image_path after a load - measured on the GKD
		 * 2026-10-03, accepted and ignored for the .m3u path. Nothing has
		 * run, so nothing is watching the lid and it closes at once. */
		if (disc >= 0 && (unsigned)disc != c->disk.get_image_index()) {
			if (diatom_disk_swap(c, (unsigned)disc)) {
				c->disk.set_eject_state(false);
				c->disk_close_in = 0;
				fprintf(stderr, "diatom: disk: starting on image %u\n",
				        c->disk.get_image_index());
			} else {
				fprintf(stderr, "diatom: disk: cannot start on image %d\n", disc);
			}
		}
	}
	/* Before the first frame can be presented, and after the core has had
	 * every chance to speak: whatever it declared is this core's, and its
	 * silence means it is still what it said the first time. */
	diatom_env_pixfmt_settle(c);

	/* Now, and not before: a core declares its memory map during load, and
	 * retro_get_memory_data has nothing to hand back until there is a game.
	 * A map that arrives later still counts - cheevos.c re-resolves. */
	diatom_cheevos_resolve(c);
	return true;
}

/* Stops the GAME, not the core. The core stays initialized and stays mapped -
 * that is the whole of ADR-0006. */
void diatom_core_stop(diatom_core *c)
{
	if (c->game_loaded) {
		c->unload_game();
		c->game_loaded = false;
	}
	free(c->content);
	c->content = NULL;
}

/* Disc swapping, plorpos-gkd.47. Only while a game is loaded: the callbacks
 * are the core's, but what they index is that game's image list. */
bool diatom_disk_report(diatom_core *c, unsigned *index, unsigned *count,
                        bool *open, char *label, size_t label_n)
{
	char *p;

	if (label_n) label[0] = '\0';
	if (!c || !c->game_loaded || !c->has_disk) return false;
	*index = c->disk.get_image_index();
	*count = c->disk.get_num_images();
	*open  = c->disk.get_eject_state();
	if (label_n && c->disk_ext && c->disk.get_image_label &&
	    !c->disk.get_image_label(*index, label, label_n))
		label[0] = '\0';
	/* It goes out on a line of tab-separated fields. */
	for (p = label; *p; p++)
		if (*p == '\t' || *p == '\n' || *p == '\r') *p = ' ';
	return true;
}

/* Open the tray and select `index`. The tray is NOT closed here: a lid that
 * opens and shuts between two frames is one the game never saw open, and a
 * PlayStation game waiting at "insert disc 2" polls for exactly that. It
 * closes in diatom_disk_tick, after frames have run. */
bool diatom_disk_swap(diatom_core *c, unsigned index)
{
	if (!c || !c->game_loaded || !c->has_disk) return false;
	if (index >= c->disk.get_num_images()) return false;
	if (!c->disk.get_eject_state() && !c->disk.set_eject_state(true)) {
		fprintf(stderr, "diatom: disk: core refused to open the tray\n");
		return false;
	}
	if (!c->disk.set_image_index(index)) {
		fprintf(stderr, "diatom: disk: core refused image %u\n", index);
		c->disk.set_eject_state(false);   /* back as it was */
		c->disk_close_in = 0;
		return false;
	}
	c->disk_close_in = DIATOM_DISC_OPEN_FRAMES;
	fprintf(stderr, "diatom: disk: image %u selected, tray open\n", index);
	return true;
}

/* Once per frame of forward play. */
void diatom_disk_tick(diatom_core *c)
{
	if (!c || c->disk_close_in <= 0 || --c->disk_close_in > 0) return;
	if (c->game_loaded && c->has_disk) {
		c->disk.set_eject_state(false);
		fprintf(stderr, "diatom: disk: tray closed on image %u\n",
		        c->disk.get_image_index());
	}
}
