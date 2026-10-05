/* SPDX-License-Identifier: MIT */
/* glswitch - can one process hand the Brick's panel between its own fbdev
 * pages and its own EGL window, and back, repeatedly (plorpos-reo.4.1)?
 *
 *   glswitch cycles <n> [frames] [shader.glsl]
 *   glswitch peer [frames]
 *
 * cycles: n times over - pan `frames` pages of fb0, then bring up SDL video, a
 * fullscreen GLES 3.0 window (as port/gkd.c makes it, swap interval 0) and
 * draw `frames` frames through gkd_gl, then tear all of that down. Never both
 * at once: every pan here is synchronous, so nothing is in flight when the
 * window comes up, and the window is gone before the next pan. fb0 stays open
 * and mapped throughout, as a paused Diatom keeps it (display-handoff spike).
 * Both halves draw a bar that sweeps across the panel at 60 Hz pacing - a
 * torn frame shows as a broken bar. One TSV row per phase on stdout.
 *
 * peer: the launcher's stand-in. Presents `frames` frames through SDL's
 * renderer, prints "idle", then keeps its window without presenting until
 * SIGUSR1, presents `frames` more and exits - so a run can check that a
 * second process's idle EGL window survives this one's switching.
 *
 * Run ONLY under tools/brick-run.sh --exec, with the launcher stopped. */
#include <errno.h>
#include <pthread.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <SDL.h>
#define SDL_USE_BUILTIN_OPENGL_DEFINITIONS 1
#include <SDL_opengles2.h>

#include "gkd_gl.h"

#define SRC_W 320
#define SRC_H 240
#define BAR   24

static double now_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

/* Self-paced at 60 Hz on an absolute clock, as Diatom's frame loop is. */
static void pace(double t0, int i)
{
	double due = t0 + (i + 1) * (1000.0 / 60.0), d = due - now_ms();

	if (d > 0) usleep((useconds_t)(d * 1000));
}

typedef struct { double sum, max; int n, late; } stat;

static void st_add(stat *s, double v)
{
	s->sum += v; s->n++;
	if (v > s->max) s->max = v;
	if (v > 1000.0 / 60.0 + 4) s->late++;
}

/* ---- display-engine layers, from sysfs: when does a GL window take a layer
 * and what does it show first (the stale-frame flash, reo.4.1 run 1)? ---- */
static double g_t0;
static void mark(const char *what)
{
	fprintf(stderr, "%9.1f mark %s\n", now_ms() - g_t0, what);
}

static void *layer_watch(void *arg)
{
	char prev[2048] = "", cur[2048];
	(void)arg;
	for (;;) {
		FILE *f = fopen("/sys/class/disp/disp/attr/sys", "r");
		char line[512];
		size_t n = 0;

		if (!f) return NULL;
		cur[0] = 0;
		while (fgets(line, sizeof line, f))
			if (strstr(line, "lyr[") && n + strlen(line) < sizeof cur) {
				strcpy(cur + n, line);
				n += strlen(line);
			}
		fclose(f);
		if (strcmp(cur, prev)) {
			fprintf(stderr, "%9.1f layers:\n%s", now_ms() - g_t0, cur);
			strcpy(prev, cur);
		}
		usleep(2000);
	}
}

/* ---- fbdev half ---- */
static int g_fd = -1;
static uint8_t *g_fb;
static struct fb_var_screeninfo g_v;
static struct fb_fix_screeninfo g_f;

static int fb_open(void)
{
	g_fd = open("/dev/fb0", O_RDWR);
	if (g_fd < 0 || ioctl(g_fd, FBIOGET_FSCREENINFO, &g_f) || ioctl(g_fd, FBIOGET_VSCREENINFO, &g_v)) {
		perror("fb0");
		return -1;
	}
	if (g_v.bits_per_pixel != 32 || g_f.smem_len < 3 * g_v.yres * g_f.line_length) {
		fprintf(stderr, "fb0: want 32bpp and 3 pages\n");
		return -1;
	}
	g_fb = mmap(NULL, g_f.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, g_fd, 0);
	if (g_fb == MAP_FAILED) { perror("mmap"); return -1; }
	return 0;
}

static void fb_phase(int cycle, int frames)
{
	stat pan = { 0 }, iv = { 0 };
	double t0 = now_ms(), last = 0;
	int fails = 0, i;

	for (i = 0; i < frames; i++) {
		int page = i % 3, x0 = (i * 8) % (int)g_v.xres, y;
		uint8_t *base = g_fb + (size_t)page * g_v.yres * g_f.line_length;
		struct fb_var_screeninfo v = g_v;
		double a, b;

		/* Opaque: a zero alpha byte is invisible on this panel. */
		for (y = 0; y < (int)g_v.yres; y++) {
			uint32_t *row = (uint32_t *)(base + (size_t)y * g_f.line_length);
			int x;

			for (x = 0; x < (int)g_v.xres; x++)
				row[x] = (x >= x0 && x < x0 + BAR) ? 0xffffffffu : 0xff202040u;
		}
		v.yoffset = (uint32_t)page * g_v.yres;
		v.activate = FB_ACTIVATE_VBL;
		a = now_ms();
		if (ioctl(g_fd, FBIOPAN_DISPLAY, &v)) fails++;
		if (!i) mark("fb first pan");
		b = now_ms();
		st_add(&pan, b - a);
		if (i) st_add(&iv, b - last);
		last = b;
		pace(t0, i);
	}
	printf("%d\tfbdev\t%d\t%.1f\t%.2f\t%.2f\t-\t-\t%.2f\t%.2f\t%d\t%d\n", cycle, frames,
	       frames * 1000.0 / (now_ms() - t0), pan.sum / pan.n, pan.max,
	       iv.sum / iv.n, iv.max, iv.late, fails);
	fflush(stdout);
}

/* ---- GL half ---- */
static void gl_phase(int cycle, int frames, const char *shader)
{
	static uint16_t src[SRC_W * SRC_H];
	SDL_Window *w;
	SDL_GLContext c;
	SDL_DisplayMode dm = { 0 };
	stat ready = { 0 }, swap = { 0 }, iv = { 0 };
	double a, up_ms, down_ms, t0, last = 0;
	void (*finish)(void);
	int sw, sh, i;
	struct fb_var_screeninfo after;

	mark("gl up begin");
	a = now_ms();
	if (SDL_InitSubSystem(SDL_INIT_VIDEO)) { fprintf(stderr, "video: %s\n", SDL_GetError()); exit(1); }
	SDL_GetDesktopDisplayMode(0, &dm);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
	SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 0);
	w = SDL_CreateWindow("glswitch", 0, 0, dm.w, dm.h,
	                     SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN | SDL_WINDOW_SHOWN);
	c = w ? SDL_GL_CreateContext(w) : NULL;
	if (!c || !gkdgl_init()) { fprintf(stderr, "gl: %s\n", SDL_GetError()); exit(1); }
	mark("gl context made");
	SDL_GL_SetSwapInterval(0);
	SDL_GL_GetDrawableSize(w, &sw, &sh);
	/* GLSWITCH_PRIME=n: n black frames before the first real one, so a
	 * swapchain buffer holding someone else's old frame is overwritten. */
	if (getenv("GLSWITCH_PRIME")) {
		void (*clr)(GLbitfield) = (void (*)(GLbitfield))SDL_GL_GetProcAddress("glClear");
		void (*cc)(GLfloat, GLfloat, GLfloat, GLfloat) =
			(void (*)(GLfloat, GLfloat, GLfloat, GLfloat))SDL_GL_GetProcAddress("glClearColor");
		int k, np = atoi(getenv("GLSWITCH_PRIME"));

		cc(0, 0, 0, 1);
		for (k = 0; k < np; k++) { clr(GL_COLOR_BUFFER_BIT); SDL_GL_SwapWindow(w); }
		mark("gl primed");
	}
	if (shader) {
		gkdgl_pass p = { shader, false, 0 };
		char err[512];

		if (!gkdgl_set_chain(&p, 1, false, err, sizeof err)) { fprintf(stderr, "chain: %s\n", err); exit(1); }
	}
	finish = (void (*)(void))SDL_GL_GetProcAddress("glFinish");
	up_ms = now_ms() - a;

	t0 = now_ms();
	for (i = 0; i < frames; i++) {
		int x0 = (i * 8 * SRC_W / sw) % SRC_W, x, y;
		diatom_rect dst = { 0, 0, sw, sh };
		double b, d;

		for (y = 0; y < SRC_H; y++)
			for (x = 0; x < SRC_W; x++)
				src[y * SRC_W + x] = (x >= x0 && x < x0 + BAR * SRC_W / sw + 1) ? 0xffff : 0x2108;
		b = now_ms();
		gkdgl_upload(src, SRC_W, SRC_H, SRC_W * 2, DIATOM_PIX_RGB565);
		gkdgl_draw(sw, sh, dst, false);
		finish();
		d = now_ms();
		st_add(&ready, d - b);
		SDL_GL_SwapWindow(w);
		if (!i) mark("gl first swap");
		b = now_ms();
		st_add(&swap, b - d);
		if (i) st_add(&iv, b - last);
		last = b;
		pace(t0, i);
	}
	/* GLSWITCH_DUMP: the last frame drawn again and read back - how much of
	 * it is not black, and the middle pixel - so a shader that draws nothing
	 * on this GPU shows up without anyone looking. */
	if (getenv("GLSWITCH_DUMP")) {
		uint8_t *px = malloc((size_t)sw * sh * 4);
		diatom_rect dst = { 0, 0, sw, sh };
		size_t k, lit = 0;

		gkdgl_draw(sw, sh, dst, false);
		if (px && gkdgl_read(sw, sh, px)) {
			for (k = 0; k < (size_t)sw * sh; k++)
				if (px[k * 4] > 8 || px[k * 4 + 1] > 8 || px[k * 4 + 2] > 8) lit++;
			k = ((size_t)(sh / 2) * sw + sw / 2) * 4;
			printf("dump\t%s\tlit %zu of %d\tmid %u,%u,%u\n", shader ? shader : "none",
			       lit, sw * sh, px[k], px[k + 1], px[k + 2]);
		}
		free(px);
	}
	mark("gl down begin");
	a = now_ms();
	gkdgl_shutdown();
	SDL_GL_DeleteContext(c);
	SDL_DestroyWindow(w);
	SDL_QuitSubSystem(SDL_INIT_VIDEO);
	down_ms = now_ms() - a;
	mark("gl down end");
	ioctl(g_fd, FBIOGET_VSCREENINFO, &after);
	printf("%d\tgl\t%d\t%.1f\t%.2f\t%.2f\t%.1f\t%.1f\t%.2f\t%.2f\t%d\tyoff=%u\n", cycle, frames,
	       frames * 1000.0 / (a - t0), swap.sum / swap.n, swap.max, up_ms, down_ms,
	       iv.sum / iv.n, iv.max, iv.late, after.yoffset);
	fprintf(stderr, "cycle %d: %dx%d ready avg %.2f max %.2f ms\n", cycle, sw, sh,
	        ready.sum / ready.n, ready.max);
	fflush(stdout);
}

/* ---- peer ---- */
static volatile sig_atomic_t g_go;
static void on_usr1(int s) { (void)s; g_go = 1; }

static void peer_frames(SDL_Renderer *r, int frames, int tint)
{
	int i;

	for (i = 0; i < frames; i++) {
		SDL_SetRenderDrawColor(r, tint ? 200 : 20, tint ? 20 : 200, 60, 255);
		SDL_RenderClear(r);
		SDL_RenderPresent(r);
	}
}

static int peer(int frames)
{
	SDL_Window *w;
	SDL_Renderer *r;
	SDL_DisplayMode dm;

	signal(SIGUSR1, on_usr1);
	if (SDL_Init(SDL_INIT_VIDEO) || SDL_GetCurrentDisplayMode(0, &dm)) return 1;
	w = SDL_CreateWindow("peer", 0, 0, dm.w, dm.h, SDL_WINDOW_FULLSCREEN);
	r = w ? SDL_CreateRenderer(w, -1, 0) : NULL;
	if (!r) { fprintf(stderr, "peer: %s\n", SDL_GetError()); return 1; }
	peer_frames(r, frames, 0);
	printf("idle\n");
	fflush(stdout);
	while (!g_go) pause();
	peer_frames(r, frames, 1);
	printf("peer: presented again, ok\n");
	SDL_DestroyRenderer(r);
	SDL_DestroyWindow(w);
	SDL_Quit();
	return 0;
}

int main(int argc, char **argv)
{
	int n, frames, i;

	if (argc > 1 && !strcmp(argv[1], "peer"))
		return peer(argc > 2 ? atoi(argv[2]) : 60);
	if (argc < 3 || strcmp(argv[1], "cycles")) {
		fprintf(stderr, "usage: glswitch cycles <n> [frames] [shader.glsl] | peer [frames]\n");
		return 2;
	}
	n = atoi(argv[2]);
	frames = argc > 3 ? atoi(argv[3]) : 180;
	if (fb_open()) return 1;
	g_t0 = now_ms();
	if (getenv("GLSWITCH_LAYERS")) {
		pthread_t t;

		pthread_create(&t, NULL, layer_watch, NULL);
	}
	printf("cycle\tphase\tframes\tfps\tpan_or_swap_avg\tpan_or_swap_max\tup_ms\tdown_ms\tiv_avg\tiv_max\tlate\textra\n");
	for (i = 0; i < n; i++) {
		fb_phase(i, frames);
		gl_phase(i, frames, argc > 4 ? argv[4] : NULL);
	}
	fb_phase(n, frames);
	return 0;
}
