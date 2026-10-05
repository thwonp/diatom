/* SPDX-License-Identifier: MIT */
/* See shot.h. */
#include "shot.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "diatom_port.h"
#include "stb_image_write.h"

bool shot_write_bmp(const char *path, const uint8_t *rgb, int w, int h)
{
	unsigned char hdr[54] = { 'B', 'M' };
	size_t row = (size_t)w * 3, pad = (4 - (row & 3)) & 3;
	unsigned int img = (unsigned int)((row + pad) * (size_t)h), size = 54 + img;
	static const unsigned char zero[3];
	FILE *f;
	int y, x;
	bool ok;

#define LE32(o, v) do { hdr[o] = (unsigned char)(v); hdr[o + 1] = (unsigned char)((v) >> 8); \
	hdr[o + 2] = (unsigned char)((v) >> 16); hdr[o + 3] = (unsigned char)((v) >> 24); } while (0)
	LE32(2, size);
	LE32(10, 54u);
	LE32(14, 40u);
	LE32(18, (unsigned int)w);
	LE32(22, (unsigned int)-h);      /* negative: rows top first */
	hdr[26] = 1;
	hdr[28] = 24;
	LE32(34, img);
#undef LE32

	f = fopen(path, "wb");
	if (!f) return false;
	ok = fwrite(hdr, 1, sizeof hdr, f) == sizeof hdr;
	for (y = 0; ok && y < h; y++) {
		const uint8_t *s = rgb + (size_t)y * row;
		unsigned char bgr[3];
		for (x = 0; ok && x < w; x++, s += 3) {
			bgr[0] = s[2]; bgr[1] = s[1]; bgr[2] = s[0];
			ok = fwrite(bgr, 1, 3, f) == 3;
		}
		if (ok && pad) ok = fwrite(zero, 1, pad, f) == pad;
	}
	return fclose(f) == 0 && ok;
}

bool shot_capture_bmp(const char *path)
{
	uint8_t *rgb;
	int w, h;
	bool ok;

	if (!path || !diatom_port_grab(&rgb, &w, &h)) return false;
	ok = shot_write_bmp(path, rgb, w, h);
	free(rgb);
	return ok;
}

/* ---- the hotkey ---------------------------------------------------------- */

typedef struct {
	uint8_t *rgb;
	int      w, h;
	uint64_t grab_us;
	bool     ok;
	char     path[1024];
} shot_job;

enum { IDLE, WRITING, DONE };
static atomic_int g_state;
static pthread_t  g_thread;
static bool       g_joinable;     /* g_thread is one to join */
static shot_job   g_job;

/* mkdir -p: the folder is the launcher's to name and nobody's to make
 * beforehand, so the first screenshot on a fresh card makes it. */
static bool make_dirs(const char *dir)
{
	char buf[512];
	char *p;

	if (snprintf(buf, sizeof buf, "%s", dir) >= (int)sizeof buf) return false;
	for (p = buf + 1; *p; p++) {
		if (*p != '/') continue;
		*p = '\0';
		if (mkdir(buf, 0755) != 0 && errno != EEXIST) return false;
		*p = '/';
	}
	return mkdir(buf, 0755) == 0 || errno == EEXIST;
}

/* stb's callback has no way to fail; a short write leaves ferror set. */
static void put(void *ctx, void *data, int size)
{
	fwrite(data, 1, (size_t)size, ctx);
}

/* To <path>.part, synced, then renamed: a card pulled or a battery gone
 * mid-write leaves a .part, never a PNG that will not open. */
static void *writer(void *arg)
{
	shot_job *j = arg;
	char part[1100];
	uint64_t t0 = diatom_port_now_us();
	bool ok = false;
	FILE *f;

	snprintf(part, sizeof part, "%s.part", j->path);
	f = fopen(part, "wb");
	if (f) {
		ok = stbi_write_png_to_func(put, f, j->w, j->h, 3, j->rgb, j->w * 3) != 0;
		ok = fflush(f) == 0 && ok && !ferror(f);
		ok = fsync(fileno(f)) == 0 && ok;
		ok = fclose(f) == 0 && ok;
		ok = ok && rename(part, j->path) == 0;
		if (!ok) unlink(part);
	}
	fprintf(stderr, "diatom: screenshot %s %s (%dx%d, grab %.1f ms, write %.0f ms)\n",
	        ok ? "saved" : "FAILED", j->path, j->w, j->h,
	        j->grab_us / 1000.0, (diatom_port_now_us() - t0) / 1000.0);
	free(j->rgb);
	j->rgb = NULL;
	j->ok = ok;
	atomic_store(&g_state, DONE);
	return NULL;
}

/* <dir>/<stem>-YYYYMMDD-HHMMSS.png, and -2, -3... after it if two shots
 * land in one second. */
static bool name_shot(char *out, size_t cap, const char *dir, const char *rom)
{
	const char *base = rom ? strrchr(rom, '/') : NULL;
	char stem[256], when[32];
	const char *dot;
	time_t now = time(NULL);
	struct tm tm;
	int n;

	base = base ? base + 1 : (rom ? rom : "diatom");
	dot = strrchr(base, '.');
	snprintf(stem, sizeof stem, "%.*s", dot && dot != base ? (int)(dot - base) : (int)strlen(base), base);
	localtime_r(&now, &tm);
	strftime(when, sizeof when, "%Y%m%d-%H%M%S", &tm);
	for (n = 1; n < 100; n++) {
		int len = n == 1 ? snprintf(out, cap, "%s/%s-%s.png", dir, stem, when)
		                 : snprintf(out, cap, "%s/%s-%s-%d.png", dir, stem, when, n);
		if (len < 0 || (size_t)len >= cap) return false;
		if (access(out, F_OK) != 0) return true;
	}
	return false;
}

bool shot_take(const char *dir, const char *rom)
{
	uint64_t t0;

	if (!dir || !*dir) return false;
	if (atomic_load(&g_state) == WRITING) {
		fprintf(stderr, "diatom: screenshot dropped, the last one is still being written\n");
		return false;
	}
	shot_wait();
	if (!make_dirs(dir)) {
		fprintf(stderr, "diatom: screenshot FAILED, cannot make %s: %s\n", dir, strerror(errno));
		return false;
	}
	if (!name_shot(g_job.path, sizeof g_job.path, dir, rom)) return false;
	t0 = diatom_port_now_us();
	if (!diatom_port_grab(&g_job.rgb, &g_job.w, &g_job.h)) {
		fprintf(stderr, "diatom: screenshot FAILED, nothing to grab\n");
		return false;
	}
	g_job.grab_us = diatom_port_now_us() - t0;
	atomic_store(&g_state, WRITING);
	g_joinable = pthread_create(&g_thread, NULL, writer, &g_job) == 0;
	if (!g_joinable) writer(&g_job);     /* no thread: write it here instead */
	return true;
}

bool shot_done(bool *ok, const char **path)
{
	if (atomic_load(&g_state) != DONE) return false;
	shot_wait();
	*ok = g_job.ok;
	*path = g_job.path;
	return true;
}

void shot_wait(void)
{
	if (atomic_load(&g_state) == IDLE) return;
	if (g_joinable) pthread_join(g_thread, NULL);
	g_joinable = false;
	atomic_store(&g_state, IDLE);
}
