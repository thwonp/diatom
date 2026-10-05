/* SPDX-License-Identifier: MIT */
/* shaderbench - what each shader costs on this GPU (plorpos-gkd.72.1).
 *
 *   shaderbench <dir with NextUI's glsl/> [frames]
 *   shaderbench sdl [frames]           today's SDL_Renderer path, same timing
 *   shaderbench selftest <shader dir>  pixels where they belong, on glass too
 *
 * Opens a fullscreen GLES window the way the GKD port does, and for every
 * shader chain x source size draws `frames` frames paced at 60 Hz, as a game
 * would: upload a changing frame, run the chain into the whole surface (the
 * worst case - Stretch), glFinish, swap. Per frame it times
 *
 *   ready = upload + every pass, until the GPU has finished (glFinish), and
 *   swap  = SDL_GL_SwapWindow after that.
 *
 * `ready` is what a shader adds between reading the pad and a finished frame:
 * its excess over None is the shader's added input latency, before sway's
 * own fixed composite. Paced rather than flat out, so the GPU's clock is the
 * one it would run a game at.
 *
 * Aborts itself past 300 MB resident (memory cap on device runs).
 * SHADERBENCH_ONLY=<prefix> runs only the chains named so ("none").
 * TSV on stdout; progress on stderr. SHADERBENCH_WINDOW=WxH for a window
 * instead of fullscreen (desktop, under xvfb).
 *
 *   shaderbench pbuffer <shader dir> [frames]
 *
 * The Brick's candidate (plorpos-reo.4, option A): no window and no SDL video
 * at all, so nothing here touches the display - an EGL pbuffer the panel's
 * size (SHADERBENCH_PBUFFER=WxH, 1024x768) is the surface, and in place of
 * the swap each frame is read back with glReadPixels, as diatom would read it
 * into an fbdev page. The swap column is that readback. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <SDL.h>
/* SDL's own copy of the GLES2 headers: the GKD sysroot has SDL's, not the
 * system's GLES2/, and functions come from SDL_GL_GetProcAddress anyway. */
#define SDL_USE_BUILTIN_OPENGL_DEFINITIONS 1
#include <SDL_opengles2.h>

#include "gkd_gl.h"

#include <dlfcn.h>
#include <SDL_egl.h>

/* ---- pbuffer: an offscreen EGL context, no SDL video ---- */
static void *g_egl, *g_gles;
static void *(*p_eglGetProcAddress)(const char *);

static void *pb_getproc(const char *name)
{
	void *f = p_eglGetProcAddress ? p_eglGetProcAddress(name) : NULL;
	return f ? f : dlsym(g_gles, name);
}

#define PB_LOAD(lib, name) \
	__typeof__(&name) q_##name = (__typeof__(&name))dlsym(lib, #name); \
	if (!q_##name) { fprintf(stderr, "pbuffer: no %s\n", #name); return false; }

static bool pb_open(int w, int h)
{
	g_egl = dlopen("libEGL.so.1", RTLD_NOW | RTLD_GLOBAL);
	if (!g_egl) g_egl = dlopen("libEGL.so", RTLD_NOW | RTLD_GLOBAL);
	g_gles = dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_GLOBAL);
	if (!g_gles) g_gles = dlopen("libGLESv2.so", RTLD_NOW | RTLD_GLOBAL);
	if (!g_egl || !g_gles) { fprintf(stderr, "pbuffer: %s\n", dlerror()); return false; }
	{
		PB_LOAD(g_egl, eglGetDisplay)
		PB_LOAD(g_egl, eglInitialize)
		PB_LOAD(g_egl, eglBindAPI)
		PB_LOAD(g_egl, eglChooseConfig)
		PB_LOAD(g_egl, eglCreatePbufferSurface)
		PB_LOAD(g_egl, eglCreateContext)
		PB_LOAD(g_egl, eglMakeCurrent)
		PB_LOAD(g_egl, eglGetError)
		const EGLint cfg_attr[] = {
			EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
			EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE };
		const EGLint surf_attr[] = { EGL_WIDTH, w, EGL_HEIGHT, h, EGL_NONE };
		const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
		EGLint maj = 0, min = 0, n = 0;
		EGLConfig cfg;
		EGLDisplay d = q_eglGetDisplay(EGL_DEFAULT_DISPLAY);
		EGLSurface s;
		EGLContext c;

		p_eglGetProcAddress = (void *(*)(const char *))dlsym(g_egl, "eglGetProcAddress");
		if (d == EGL_NO_DISPLAY || !q_eglInitialize(d, &maj, &min)) {
			fprintf(stderr, "pbuffer: eglInitialize 0x%x\n", q_eglGetError()); return false; }
		q_eglBindAPI(EGL_OPENGL_ES_API);
		if (!q_eglChooseConfig(d, cfg_attr, &cfg, 1, &n) || n < 1) {
			fprintf(stderr, "pbuffer: no config 0x%x\n", q_eglGetError()); return false; }
		s = q_eglCreatePbufferSurface(d, cfg, surf_attr);
		c = q_eglCreateContext(d, cfg, EGL_NO_CONTEXT, ctx_attr);
		if (s == EGL_NO_SURFACE || c == EGL_NO_CONTEXT || !q_eglMakeCurrent(d, s, s, c)) {
			fprintf(stderr, "pbuffer: surface/context 0x%x\n", q_eglGetError()); return false; }
		fprintf(stderr, "pbuffer: EGL %d.%d, %dx%d\n", maj, min, w, h);
	}
	return true;
}

#define RSS_CAP_MB 300

typedef struct {
	const char *name;
	int n;
	gkdgl_pass p[GKDGL_MAX_PASSES];
	bool final_linear, none_linear;
} chain;

static char g_dir[512];
static char g_paths[GKDGL_MAX_PASSES][600];

static const char *const SINGLES[] = {
	"aa-shader-4.0", "barrel-distortion", "bicubic", "edge1pixel", "fast-sharpen",
	"lcd1x", "lcd3x", "pixel_art_AA", "pixellate", "res-independent-scanlines",
	"retro-v2", "scale3x", "scanline", "sharp-bilinear", "sharp-shimmerless",
	"sharp-shimmerless-grid", "sharp-shimmerless-subpixel-vrgb", "stock", "waterpaint",
};

typedef struct { int w, h; diatom_pixfmt fmt; const char *what; } source;
static const source SOURCES[] = {
	{ 160, 144, DIATOM_PIX_RGB565,   "160x144 GB" },
	{ 320, 224, DIATOM_PIX_RGB565,   "320x224 MD" },
	{ 640, 480, DIATOM_PIX_XRGB8888, "640x480 PS" },
};

static uint64_t now_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static long rss_mb(void)
{
	long pages = 0, rss = 0;
	FILE *f = fopen("/proc/self/statm", "r");

	if (!f) return 0;
	if (fscanf(f, "%ld %ld", &pages, &rss) != 2) rss = 0;
	fclose(f);
	return rss * sysconf(_SC_PAGESIZE) / (1024 * 1024);
}

static int cmp_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
	return x < y ? -1 : x > y;
}

static const char *path(int slot, const char *name)
{
	snprintf(g_paths[slot], sizeof g_paths[slot], "%s/glsl/%s.glsl", g_dir, name);
	return g_paths[slot];
}

/* Noise, so no shader gets an easy flat frame. */
static void fill(uint8_t *buf, const source *s, unsigned frame)
{
	int bpp = s->fmt == DIATOM_PIX_RGB565 ? 2 : 4;
	size_t n = (size_t)s->w * (size_t)s->h * (size_t)bpp, i;
	uint32_t x = 2463534242u ^ frame;

	for (i = 0; i < n; i++) {
		x ^= x << 13; x ^= x >> 17; x ^= x << 5;
		buf[i] = (uint8_t)x;
	}
}

/* ---- sdl: the SDL_Renderer present port/gkd.c used before plorpos-gkd.72,
 * timed exactly as the chains are, so None can be held against it. ---- */
static int run_sdl(int frames)
{
	SDL_DisplayMode dm = { 0 };
	SDL_Window *win;
	SDL_Renderer *ren;
	void (*gl_finish)(void);
	uint8_t *buf = malloc(640 * 480 * 4);
	uint64_t *ready = malloc(sizeof *ready * (size_t)frames);
	uint64_t *swap = malloc(sizeof *swap * (size_t)frames);
	int sw = 0, sh = 0, si;

	if (!buf || !ready || !swap) return 1;
	if (SDL_GetDesktopDisplayMode(0, &dm) != 0) { dm.w = 0; dm.h = 0; }
	win = SDL_CreateWindow("shaderbench", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
	                       dm.w, dm.h, SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_SHOWN);
	ren = win ? SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED) : NULL;
	if (!ren) { fprintf(stderr, "renderer: %s\n", SDL_GetError()); return 1; }
	SDL_RenderSetVSync(ren, 0);
	for (int i = 0; i < 100; i++) {
		SDL_PumpEvents();
		SDL_GetRendererOutputSize(ren, &sw, &sh);
		if (sw > 1 && sh > 1) break;
		SDL_Delay(10);
	}
	gl_finish = (void (*)(void))SDL_GL_GetProcAddress("glFinish");
	if (!gl_finish) { fprintf(stderr, "no glFinish\n"); return 1; }
	fprintf(stderr, "SDL_Renderer path | surface %dx%d | %d frames each\n", sw, sh, frames);
	printf("chain\tsource\tready_med_ms\tready_p95_ms\tready_max_ms\tswap_med_ms\tlate\n");
	for (si = 0; si < (int)(sizeof SOURCES / sizeof *SOURCES); si++) {
		const source *s = &SOURCES[si];
		size_t pitch = (size_t)s->w * (s->fmt == DIATOM_PIX_RGB565 ? 2 : 4);
		SDL_Texture *tex = SDL_CreateTexture(ren,
			s->fmt == DIATOM_PIX_RGB565 ? SDL_PIXELFORMAT_RGB565 : SDL_PIXELFORMAT_RGB888,
			SDL_TEXTUREACCESS_STREAMING, s->w, s->h);
		uint64_t next = now_us();
		int late = 0;

		SDL_SetTextureScaleMode(tex, SDL_ScaleModeNearest);
		for (int f = -10; f < frames; f++) {
			uint64_t t0, t1, t2;

			if (f == -10) fill(buf, s, 0);
			else buf[(size_t)((unsigned)f % (unsigned)s->h) * pitch] ^= 0xff;
			SDL_PumpEvents();
			t0 = now_us();
			SDL_UpdateTexture(tex, NULL, buf, (int)pitch);
			SDL_RenderClear(ren);
			SDL_RenderCopy(ren, tex, NULL, NULL);
			SDL_RenderFlush(ren);
			gl_finish();
			t1 = now_us();
			SDL_RenderPresent(ren);
			t2 = now_us();
			if (f >= 0) { ready[f] = t1 - t0; swap[f] = t2 - t1; }
			next += 16667;
			if (now_us() > next) { if (f >= 0) late++; next = now_us(); }
			else while (now_us() < next) usleep(500);
		}
		SDL_DestroyTexture(tex);
		qsort(ready, (size_t)frames, sizeof *ready, cmp_u64);
		qsort(swap, (size_t)frames, sizeof *swap, cmp_u64);
		printf("sdl renderer (today)\t%s\t%.2f\t%.2f\t%.2f\t%.2f\t%d\n", s->what,
		       ready[frames / 2] / 1000.0, ready[frames * 95 / 100] / 1000.0,
		       ready[frames - 1] / 1000.0, swap[frames / 2] / 1000.0, late);
	}
	SDL_DestroyRenderer(ren);
	SDL_DestroyWindow(win);
	SDL_Quit();
	return 0;
}

/* ---- selftest ---- */
static int g_fails;

static void expect(const char *what, const uint8_t *px, int r, int g, int b)
{
	int ok = abs(px[0] - r) <= 8 && abs(px[1] - g) <= 8 && abs(px[2] - b) <= 8;
	printf("  %s %-44s got %3d,%3d,%3d want %3d,%3d,%3d\n", ok ? "ok  " : "FAIL",
	       what, px[0], px[1], px[2], r, g, b);
	if (!ok) g_fails++;
}

/* Quadrants red | green over blue | white. XRGB8888's spare byte is 0 -
 * the case that once composited see-through (black) on this device. */
static void quadrants(uint8_t *buf, int w, int h, diatom_pixfmt fmt)
{
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			int q = (y >= h / 2) * 2 + (x >= w / 2);
			static const uint8_t rgb[4][3] = { {255,0,0}, {0,255,0}, {0,0,255}, {255,255,255} };
			const uint8_t *c = rgb[q];
			if (fmt == DIATOM_PIX_RGB565) {
				uint16_t v = (uint16_t)(((c[0] >> 3) << 11) | ((c[1] >> 2) << 5) | (c[2] >> 3));
				memcpy(buf + ((size_t)y * w + x) * 2, &v, 2);
			} else {
				uint8_t *d = buf + ((size_t)y * w + x) * 4;
				d[0] = c[2]; d[1] = c[1]; d[2] = c[0]; d[3] = 0;
			}
		}
}

/* Everything the port draws, at known places: the frame offset in the
 * surface, a translucent level-bar fill over it, an opaque fill, and a
 * half-transparent overlay over black. */
#define FX 100
#define FY 50
#define FW 640
#define FH 480
static void scene(int sw, int sh, diatom_pixfmt fmt, uint8_t *frame, uint8_t *ov)
{
	diatom_rect dst = { FX, FY, FW, FH };
	diatom_rect half = { FX + 3 * FW / 4 - 40, FY + FH / 4 - 40, 80, 80 };
	diatom_rect solid = { 1300, 100, 100, 80 };
	diatom_rect ovr = { 1000, 900, 200, 100 };

	quadrants(frame, 64, 48, fmt);
	gkdgl_upload(frame, 64, 48, (size_t)64 * (fmt == DIATOM_PIX_RGB565 ? 2 : 4), fmt);
	gkdgl_draw(sw, sh, dst, false);
	gkdgl_fill(sw, sh, half, 0, 0, 0, 128);
	gkdgl_fill(sw, sh, solid, 255, 255, 0, 255);
	gkdgl_overlay_set(ov, 200, 100);
	gkdgl_overlay_draw(sw, sh, ovr);
}

static void check_scene(const char *where, const uint8_t *rgba, int stride_px)
{
#define AT(x, y) (rgba + ((size_t)(y) * (size_t)stride_px + (size_t)(x)) * 4)
	char what[96];
	printf(" %s:\n", where);
	snprintf(what, sizeof what, "outside the frame is black");
	expect(what, AT(20, 20), 0, 0, 0);
	expect("frame top-left quadrant is red",       AT(FX + FW / 4, FY + FH / 4 + 60), 255, 0, 0);
	expect("frame top-right quadrant is green",    AT(FX + 3 * FW / 4, FY + 3 * FH / 8 + 40), 0, 255, 0);
	expect("frame bottom-left quadrant is blue",   AT(FX + FW / 4, FY + 3 * FH / 4), 0, 0, 255);
	expect("frame bottom-right quadrant is white", AT(FX + 3 * FW / 4, FY + 3 * FH / 4), 255, 255, 255);
	expect("frame starts at x=100 (99 is black)",  AT(FX - 1, FY + 10), 0, 0, 0);
	expect("frame starts at y=50 (49 is black)",   AT(FX + 10, FY - 1), 0, 0, 0);
	expect("frame's first pixel is red",           AT(FX, FY), 255, 0, 0);
	expect("half-black fill over green",           AT(FX + 3 * FW / 4, FY + FH / 4), 0, 128, 0);
	expect("opaque yellow fill",                   AT(1350, 140), 255, 255, 0);
	expect("half-transparent magenta overlay",     AT(1100, 950), 128, 0, 128);
#undef AT
}

static uint8_t *read_ppm(const char *path, int *w, int *h)
{
	FILE *f = fopen(path, "rb");
	uint8_t *rgb, *rgba;
	int maxv;

	if (!f) return NULL;
	if (fscanf(f, "P6 %d %d %d", w, h, &maxv) != 3 || maxv != 255) { fclose(f); return NULL; }
	fgetc(f);
	rgb = malloc((size_t)*w * (size_t)*h * 3);
	rgba = malloc((size_t)*w * (size_t)*h * 4);
	if (!rgb || !rgba || fread(rgb, 3, (size_t)*w * (size_t)*h, f) != (size_t)*w * (size_t)*h) {
		fclose(f); free(rgb); free(rgba); return NULL;
	}
	fclose(f);
	for (size_t i = 0; i < (size_t)*w * (size_t)*h; i++) {
		rgba[i * 4] = rgb[i * 3]; rgba[i * 4 + 1] = rgb[i * 3 + 1];
		rgba[i * 4 + 2] = rgb[i * 3 + 2]; rgba[i * 4 + 3] = 255;
	}
	free(rgb);
	return rgba;
}

static int run_selftest(SDL_Window *win, int sw, int sh)
{
	uint8_t *frame = malloc(64 * 48 * 4), *ov = malloc(200 * 100 * 4);
	uint8_t *rgba = malloc((size_t)sw * (size_t)sh * 4);
	static const diatom_pixfmt fmts[2] = { DIATOM_PIX_RGB565, DIATOM_PIX_XRGB8888 };
	char err[600];

	if (!frame || !ov || !rgba) return 1;
	for (int i = 0; i < 200 * 100; i++) {   /* magenta, alpha 128, BGRA */
		ov[i * 4] = 255; ov[i * 4 + 1] = 0; ov[i * 4 + 2] = 255; ov[i * 4 + 3] = 128;
	}
	for (int k = 0; k < 2; k++) {
		const char *name = fmts[k] == DIATOM_PIX_RGB565 ? "RGB565" : "XRGB8888";
		char where[64];

		printf("%s frame, None:\n", name);
		scene(sw, sh, fmts[k], frame, ov);
		snprintf(where, sizeof where, "GL readback");
		if (gkdgl_read(sw, sh, rgba)) check_scene(where, rgba, sw);
		else { printf("  FAIL readback\n"); g_fails++; }

		/* On glass: keep it up long enough for sway to have made the window
		 * fullscreen and shown it (0.3 s was not: grim saw the launcher). */
		for (int f = 0; f < 120; f++) {
			scene(sw, sh, fmts[k], frame, ov);
			SDL_GL_SwapWindow(win);
			SDL_PumpEvents();
			SDL_Delay(16);
		}
		if (system("grim -t ppm /tmp/shaderbench-glass.ppm") == 0) {
			int gw = 0, gh = 0;
			uint8_t *g = read_ppm("/tmp/shaderbench-glass.ppm", &gw, &gh);
			if (g && gw == sw && gh == sh) check_scene("on glass (grim)", g, gw);
			else { printf("  FAIL grim image %dx%d, surface %dx%d\n", gw, gh, sw, sh); g_fails++; }
			free(g);
		} else {
			printf("  skip on glass: no grim here\n");
		}
	}

	/* What a resident does over days, compressed: frame sizes and chains
	 * changing under it. Sampled per phase, so a one-off driver pool shows
	 * as a step and a leak as a slope: memory must stop growing. */
	{
		static const char *const phase[3] = { "steady (one size, None)",
			"size changes every frame", "chain changes every frame" };
		long first = 0, last = 0, mid = 0;

		for (int ph = 0; ph < 3; ph++) {
			long start = rss_mb();
			for (int i = 0; i < 1200; i++) {
				int w = ph == 1 ? 64 + (i * 37) % 577 : 320;
				int h = ph == 1 ? 64 + (i * 53) % 417 : 224;
				diatom_pixfmt fmt = ph == 1 ? fmts[i & 1] : DIATOM_PIX_RGB565;
				diatom_rect dst = { 0, 0, sw, sh };
				uint8_t *big = malloc((size_t)w * (size_t)h * 4);

				if (ph == 2) {
					gkdgl_pass p[2] = { { path(0, "sharp-bilinear"), true, 0 },
					                    { path(1, "lcd3x"), false, 0 } };
					if (!gkdgl_set_chain(p, i % 3, false, err, sizeof err)) {
						printf("  FAIL chain: %s\n", err); g_fails++; }
				}
				if (!big) break;
				memset(big, i, (size_t)w * (size_t)h * 4);
				gkdgl_upload(big, w, h, (size_t)w * (fmt == DIATOM_PIX_RGB565 ? 2 : 4), fmt);
				gkdgl_draw(sw, sh, dst, false);
				gkdgl_overlay_set(i % 3 ? ov : NULL, 200, 100);
				SDL_GL_SwapWindow(win);
				SDL_PumpEvents();
				free(big);
				if (i == 399) mid = rss_mb();
			}
			last = rss_mb();
			printf("%-28s rss %ld -> %ld (after 400) -> %ld MB\n", phase[ph], start, mid, last);
			if (ph == 0) first = start;
			/* Past the first 400 frames of a phase, nothing should grow. */
			if (last - mid > 3) { printf("  FAIL still growing in the last 800 frames\n"); g_fails++; }
			else printf("  ok   level after warm-up\n");
		}
		(void)first;
	}

	printf(g_fails ? "SELFTEST: %d FAILED\n" : "SELFTEST: all passed\n", g_fails);
	return g_fails ? 1 : 0;
}

int main(int argc, char **argv)
{
	SDL_Window *win;
	SDL_GLContext ctx;
	void (*gl_finish)(void);
	const GLubyte *(*gl_string)(GLenum);
	int frames = argc > 2 ? atoi(argv[2]) : 120;
	int sw = 0, sh = 0, ww = 0, wh = 0;
	chain chains[2 + 19 + 4 + 2];
	int nchains = 0, c, si;
	uint8_t *buf;
	uint64_t *ready, *swap;
	const char *win_env = getenv("SHADERBENCH_WINDOW");

	bool selftest = argc > 2 && !strcmp(argv[1], "selftest");
	bool pbuffer = argc > 2 && !strcmp(argv[1], "pbuffer");
	uint8_t *rb = NULL;
	unsigned rb_fmt = GL_RGBA;
	void (*gl_read)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *) = NULL;
	/* SHADERBENCH_PBO=1: read through two pixel-pack buffers, each frame
	 * starting its own read and copying out the one before it. */
	bool pbo = getenv("SHADERBENCH_PBO") != NULL;
	unsigned pbos[2] = { 0, 0 };
	void (*gl_genbuf)(GLsizei, GLuint *) = NULL;
	void (*gl_bindbuf)(GLenum, GLuint) = NULL;
	void (*gl_bufdata)(GLenum, GLsizeiptr, const void *, GLenum) = NULL;
	void *(*gl_map)(GLenum, GLintptr, GLsizeiptr, GLbitfield) = NULL;
	GLboolean (*gl_unmap)(GLenum) = NULL;

	if (argc < 2) { fprintf(stderr, "usage: shaderbench <shader dir> [frames]\n"); return 2; }
	if (frames < 10) frames = 10;
	if (pbuffer) {
		const char *sz = getenv("SHADERBENCH_PBUFFER");
		void (*gl_int)(GLenum, GLint *);
		GLint f = 0, t = 0;

		sw = 1024; sh = 768;
		if (sz) sscanf(sz, "%dx%d", &sw, &sh);
		frames = argc > 3 ? atoi(argv[3]) : 120;
		if (frames < 10) frames = 10;
		snprintf(g_dir, sizeof g_dir, "%s", argv[2]);
		if (!pb_open(sw, sh)) return 1;
		gl_finish = (void (*)(void))pb_getproc("glFinish");
		gl_string = (const GLubyte *(*)(GLenum))pb_getproc("glGetString");
		gl_read = (void (*)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *))pb_getproc("glReadPixels");
		gl_int = (void (*)(GLenum, GLint *))pb_getproc("glGetIntegerv");
		if (!gl_finish || !gl_string || !gl_read || !gl_int || !gkdgl_init_with(pb_getproc)) {
			fprintf(stderr, "GL init failed\n"); return 1; }
		gl_int(0x8B9B /* IMPLEMENTATION_COLOR_READ_FORMAT */, &f);
		gl_int(0x8B9A /* IMPLEMENTATION_COLOR_READ_TYPE */, &t);
		if (f == 0x80E1 /* BGRA_EXT */ && t == GL_UNSIGNED_BYTE) rb_fmt = 0x80E1;
		rb = malloc((size_t)sw * (size_t)sh * 4);
		if (!rb) return 1;
		if (pbo) {
			gl_genbuf = (void (*)(GLsizei, GLuint *))pb_getproc("glGenBuffers");
			gl_bindbuf = (void (*)(GLenum, GLuint))pb_getproc("glBindBuffer");
			gl_bufdata = (void (*)(GLenum, GLsizeiptr, const void *, GLenum))pb_getproc("glBufferData");
			gl_map = (void *(*)(GLenum, GLintptr, GLsizeiptr, GLbitfield))pb_getproc("glMapBufferRange");
			gl_unmap = (GLboolean (*)(GLenum))pb_getproc("glUnmapBuffer");
			if (!gl_genbuf || !gl_bindbuf || !gl_bufdata || !gl_map || !gl_unmap) {
				fprintf(stderr, "PBO: no ES3 buffer mapping\n"); return 1; }
			gl_genbuf(2, pbos);
			for (int i = 0; i < 2; i++) {
				gl_bindbuf(0x88EB /* PIXEL_PACK_BUFFER */, pbos[i]);
				gl_bufdata(0x88EB, (GLsizeiptr)sw * sh * 4, NULL, 0x88E1 /* STREAM_READ */);
			}
			gl_bindbuf(0x88EB, 0);
			fprintf(stderr, "PBO: two pack buffers, read lags one frame\n");
		}
		win = NULL; ctx = NULL;
		fprintf(stderr, "GL: %s | %s | pbuffer %dx%d | read format 0x%x type 0x%x -> %s | %d frames each\n",
		        gl_string(GL_RENDERER), gl_string(GL_VERSION), sw, sh, f, t,
		        rb_fmt == GL_RGBA ? "RGBA" : "BGRA", frames);
		goto bench;
	}
	setenv("MALI_WAYLAND_AFBC", "0", 0);   /* as port/gkd.c */
	if (SDL_Init(SDL_INIT_VIDEO) != 0) { fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1; }
	if (!strcmp(argv[1], "sdl")) return run_sdl(frames);
	snprintf(g_dir, sizeof g_dir, "%s", selftest ? argv[2] : argv[1]);

	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
	SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 0);   /* as port/gkd.c */

	if (win_env && sscanf(win_env, "%dx%d", &ww, &wh) == 2) {
		win = SDL_CreateWindow("shaderbench", 0, 0, ww, wh, SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN);
	} else {
		SDL_DisplayMode dm = { 0 };
		if (SDL_GetDesktopDisplayMode(0, &dm) != 0) { dm.w = 0; dm.h = 0; }
		win = SDL_CreateWindow("shaderbench", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
		                       dm.w, dm.h,
		                       SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_SHOWN);
	}
	if (!win) { fprintf(stderr, "window: %s\n", SDL_GetError()); return 1; }
	ctx = SDL_GL_CreateContext(win);
	if (!ctx) { fprintf(stderr, "context: %s\n", SDL_GetError()); return 1; }
	if (SDL_GL_SetSwapInterval(0) != 0)
		fprintf(stderr, "note: swap interval 0 refused: %s\n", SDL_GetError());
	for (int i = 0; i < 100; i++) {   /* sway's first configure, as port/gkd.c */
		SDL_PumpEvents();
		SDL_GL_GetDrawableSize(win, &sw, &sh);
		if (sw > 1 && sh > 1) break;
		SDL_Delay(10);
	}
	gl_finish = (void (*)(void))SDL_GL_GetProcAddress("glFinish");
	gl_string = (const GLubyte *(*)(GLenum))SDL_GL_GetProcAddress("glGetString");
	if (!gl_finish || !gl_string || !gkdgl_init()) { fprintf(stderr, "GL init failed\n"); return 1; }
	fprintf(stderr, "GL: %s | %s | surface %dx%d | %d frames each\n",
	        gl_string(GL_RENDERER), gl_string(GL_VERSION), sw, sh, frames);
	if (selftest) return run_selftest(win, sw, sh);
bench:

	/* None, both ways; every single shader into the screen; the presets'
	 * distinct chains (real-gameboy and real-gba are the same two shaders). */
	chains[nchains++] = (chain){ "none (nearest)", 0, { { 0 } }, false, false };
	chains[nchains++] = (chain){ "none (linear)",  0, { { 0 } }, false, true };
	for (c = 0; c < 19; c++) {
		chain ch = { SINGLES[c], 1, { { NULL, !strcmp(SINGLES[c], "sharp-bilinear"), 0 } }, false, false };
		chains[nchains++] = ch;
	}
	chains[nchains++] = (chain){ "preset pixelperfect-sharp", 1, { { "pixellate", false, 0 } }, false, false };
	chains[nchains++] = (chain){ "preset pixelperfect-smooth", 1, { { "stock", false, 3 } }, true, false };
	chains[nchains++] = (chain){ "preset real-gameboy/gba", 2,
		{ { "pixellate", false, 0 }, { "lcd3x", false, 0 } }, false, false };
	chains[nchains++] = (chain){ "preset old-tv", 2,
		{ { "barrel-distortion", false, 0 }, { "res-independent-scanlines", false, 0 } }, false, false };
	/* scanlines.cfg is res-independent-scanlines alone - measured as a single. */
	/* Not NextUI's: SkyWalker541's pixel transparency, alone and after lcd3x
	 * (plorpos-gkd.86.1). */
	chains[nchains++] = (chain){ "pt", 1, { { "PT_SkyWalker541", false, 0 } }, false, false };
	chains[nchains++] = (chain){ "pt after lcd3x", 2,
		{ { "lcd3x", false, 0 }, { "PT_SkyWalker541", false, 0 } }, false, false };

	buf = malloc(640 * 480 * 4);
	ready = malloc(sizeof *ready * (size_t)frames);
	swap = malloc(sizeof *swap * (size_t)frames);
	if (!buf || !ready || !swap) return 1;

	printf("chain\tsource\tready_med_ms\tready_p95_ms\tready_max_ms\tswap_med_ms\tlate\n");
	for (c = 0; c < nchains; c++) {
		chain *ch = &chains[c];
		gkdgl_pass p[GKDGL_MAX_PASSES];
		char err[600];
		const char *one = ch->n == 1 && !ch->p[0].path ? ch->name : NULL;
		const char *only = getenv("SHADERBENCH_ONLY");   /* a name prefix */

		if (only && strncmp(ch->name, only, strlen(only))) continue;

		for (int i = 0; i < ch->n; i++) {
			p[i] = ch->p[i];
			p[i].path = path(i, one ? one : ch->p[i].path);
		}
		if (!gkdgl_set_chain(p, ch->n, ch->final_linear, err, sizeof err)) {
			printf("%s\t-\tCOMPILE FAILED: %s\n", ch->name, err);
			fprintf(stderr, "%s: compile failed\n", ch->name);
			continue;
		}
		for (si = 0; si < (int)(sizeof SOURCES / sizeof *SOURCES); si++) {
			const source *s = &SOURCES[si];
			size_t pitch = (size_t)s->w * (s->fmt == DIATOM_PIX_RGB565 ? 2 : 4);
			diatom_rect dst = { 0, 0, sw, sh };
			uint64_t next = now_us();
			int late = 0;

			for (int f = -10; f < frames; f++) {   /* 10 warmup frames */
				uint64_t t0, t1, t2;

				/* One row changes per frame: the upload is still the whole
				 * frame, but the screen shows still static, not flicker. */
				if (f == -10) fill(buf, s, 0);
				else buf[(size_t)((unsigned)f % (unsigned)s->h) * pitch] ^= 0xff;
				if (!pbuffer) SDL_PumpEvents();
				t0 = now_us();
				gkdgl_upload(buf, s->w, s->h, pitch, s->fmt);
				gkdgl_draw(sw, sh, dst, ch->none_linear);
				gl_finish();
				t1 = now_us();
				if (pbuffer && pbo) {
					void *m;
					gl_bindbuf(0x88EB, pbos[f & 1]);
					gl_read(0, 0, sw, sh, rb_fmt, GL_UNSIGNED_BYTE, NULL);
					gl_bindbuf(0x88EB, pbos[(f + 1) & 1]);
					m = gl_map(0x88EB, 0, (GLintptr)sw * sh * 4, 0x0001 /* MAP_READ */);
					if (m) { memcpy(rb, m, (size_t)sw * (size_t)sh * 4); gl_unmap(0x88EB); }
					gl_bindbuf(0x88EB, 0);
				} else if (pbuffer) gl_read(0, 0, sw, sh, rb_fmt, GL_UNSIGNED_BYTE, rb);
				else SDL_GL_SwapWindow(win);
				t2 = now_us();
				if (f >= 0) { ready[f] = t1 - t0; swap[f] = t2 - t1; }

				next += 16667;
				if (now_us() > next) { if (f >= 0) late++; next = now_us(); }
				else while (now_us() < next) usleep(500);
			}
			qsort(ready, (size_t)frames, sizeof *ready, cmp_u64);
			qsort(swap, (size_t)frames, sizeof *swap, cmp_u64);
			printf("%s\t%s\t%.2f\t%.2f\t%.2f\t%.2f\t%d\n", ch->name, s->what,
			       ready[frames / 2] / 1000.0, ready[frames * 95 / 100] / 1000.0,
			       ready[frames - 1] / 1000.0, swap[frames / 2] / 1000.0, late);
			fflush(stdout);
			if (rss_mb() > RSS_CAP_MB) {
				fprintf(stderr, "RSS %ld MB > %d MB cap: stopping\n", rss_mb(), RSS_CAP_MB);
				return 3;
			}
		}
		fprintf(stderr, "%d/%d %s (rss %ld MB)\n", c + 1, nchains, ch->name, rss_mb());
	}

	gkdgl_shutdown();
	if (pbuffer) return 0;   /* the process exit takes the EGL context */
	SDL_GL_DeleteContext(ctx);
	SDL_DestroyWindow(win);
	SDL_Quit();
	return 0;
}
